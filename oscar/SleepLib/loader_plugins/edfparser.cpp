﻿/* EDF Parser Implementation
 *
 * Copyright (c) 2019 The OSCAR Team
 * Copyright (c) 2011-2018 Mark Watkins <mark@jedimark.net>
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QMutexLocker>
#ifdef _MSC_VER
#include <QtZlib/zlib.h>
#else
#include <zlib.h>
#endif

#include "edfparser.h"

EDFInfo::EDFInfo()
{
    filesize = 0;
    datasize = 0;
    signalPtr = nullptr;
    hdrPtr = nullptr;
    fileData = nullptr;
}

EDFInfo::~EDFInfo()
{
    for (auto & s : edfsignals) {
        if (s.value)  
            delete [] s.value;
    }
//    for (auto & a : annotations)
//    	delete  a;
}

QByteArray * EDFInfo::Open(const QString & name)
{
    if (hdrPtr != nullptr) {
        qWarning() << "EDFInfo::Open() called with file already open " << name;
        sleep(1);
        return nullptr;
    }
    QFile fi(name);
    if (!fi.open(QFile::ReadOnly)) {
        qDebug() << "EDFInfo::Open() Couldn't open file " << name;
        sleep(1);
        return nullptr;
    }
    fileData = new QByteArray();
    if (name.endsWith(STR_ext_gz)) {
        *fileData = gUncompress(fi.readAll()); // Open and decompress file
    } else {
        *fileData = fi.readAll(); // Open and read uncompressed file
    }
    fi.close();
    if (fileData->size() <= EDFHeaderSize) {
    	delete fileData;
        qDebug() << "EDFInfo::Open() File too short " << name;
        sleep(1);
        return nullptr;
    }
    filename = name;
    return fileData;
}

bool EDFInfo::Parse(QByteArray * fileData ) 
{
    bool ok;

    if (fileData == nullptr) {
        qWarning() << "EDFInfo::Parse() called without valid EDF data " << filename;
        sleep(1);
        return false;
    }
    
    hdrPtr = (EDFHeaderRaw *)(*fileData).constData();
    signalPtr = (char *)(*fileData).constData() + EDFHeaderSize;
    filesize = (*fileData).size();
    datasize = filesize - EDFHeaderSize;
    pos = 0;

    eof = false;
    edfHdr.version = QString::fromLatin1(hdrPtr->version, 8).toLong(&ok);
    if (!ok) {
        qWarning() << "EDFInfo::Parse() Bad Version " << filename;
        sleep(1);
        return false;
    }

    edfHdr.patientident=QString::fromLatin1(hdrPtr->patientident,80);
    edfHdr.recordingident = QString::fromLatin1(hdrPtr->recordingident, 80); // Serial number is in here..
    edfHdr.startdate_orig = QDateTime::fromString(QString::fromLatin1(hdrPtr->datetime, 16), "dd.MM.yyHH.mm.ss");
    edfHdr.num_header_bytes = QString::fromLatin1(hdrPtr->num_header_bytes, 8).toLong(&ok);
    if (!ok) {
        qWarning() << "EDFInfo::Parse() Bad header byte count " << filename;
        sleep(1);
        return false;
    }
    edfHdr.reserved44=QString::fromLatin1(hdrPtr->reserved, 44);
    edfHdr.num_data_records = QString::fromLatin1(hdrPtr->num_data_records, 8).toLong(&ok);
    if (!ok) {
        qWarning() << "EDFInfo::Parse() Bad data record count " << filename;
        sleep(1);
        return false;
    }
    edfHdr.duration_Seconds = QString::fromLatin1(hdrPtr->dur_data_records, 8).toDouble(&ok);
    if (!ok) {
        qWarning() << "EDFInfo::Parse() Bad duration " << filename;
        sleep(1);
        return false;
    }
    edfHdr.num_signals = QString::fromLatin1(hdrPtr->num_signals, 4).toLong(&ok);
    if (!ok) {
        qWarning() << "EDFInfo::Parse() Bad number of signals " << filename;
        sleep(1);
        return false;
    }

    // Initialize fixed-size signal list.
    edfsignals.resize(edfHdr.num_signals);

    // Now copy all the Signal descriptives into edfsignals
    for (auto & sig : edfsignals) {
        sig.value = nullptr;
        sig.label = ReadBytes(16);

        signal_labels.push_back(sig.label);
        signalList[sig.label].push_back(&sig);
        if (eof) {
            qWarning() << "EDFInfo::Parse() Early end of file " << filename;
            sleep(1);
            return false;
        }
    }
    for (auto & sig : edfsignals) { 
        sig.transducer_type = ReadBytes(80); 
    }
    for (auto & sig : edfsignals) { 
        sig.physical_dimension = ReadBytes(8); 
    }
    for (auto & sig : edfsignals) { 
        sig.physical_minimum = ReadBytes(8).toDouble(&ok); 
    }
    for (auto & sig : edfsignals) { 
        sig.physical_maximum = ReadBytes(8).toDouble(&ok); 
    }
    for (auto & sig : edfsignals) { 
        sig.digital_minimum = ReadBytes(8).toDouble(&ok); 
    }
    for (auto & sig : edfsignals) { 
        sig.digital_maximum = ReadBytes(8).toDouble(&ok);
        sig.gain = (sig.physical_maximum - sig.physical_minimum) / (sig.digital_maximum - sig.digital_minimum);
        sig.offset = 0;
    }
    for (auto & sig : edfsignals) { 
        sig.prefiltering = ReadBytes(80); 
    }
    for (auto & sig : edfsignals) { 
        sig.nr = ReadBytes(8).toLong(&ok); 
    }
    for (auto & sig : edfsignals) { 
        sig.reserved = ReadBytes(32); 
    }

    // could do it earlier, but it won't crash from > EOF Reads
    if (eof) {
        qWarning() << "EDFInfo::Parse() Early end of file " << filename;
        sleep(1);
        return false;
    }

    // Now check the file isn't truncated before allocating space for the values
    long allocsize = 0;
    for (auto & sig : edfsignals) {
        if (edfHdr.num_data_records > 0) {
            allocsize += sig.nr * edfHdr.num_data_records * 2;
        }
    }
    if (allocsize > (datasize - pos)) {
        // Space required more than the remainder left to read,
        // so abort and let the user clean up the corrupted file themselves
        qWarning() << "EDFInfo::Parse(): " << filename << " is too short!";
        sleep(1);
        return false;
    }

    // allocate the arrays for the signal values
    for (auto & sig : edfsignals) {
        long recs = sig.nr * edfHdr.num_data_records;
        if (edfHdr.num_data_records <= 0) {
            sig.value = nullptr;
            continue;
        }
        sig.value = new qint16 [recs];
        sig.pos = 0;
    }
    for (int x = 0; x < edfHdr.num_data_records; x++) {
        for (auto & sig : edfsignals) {
        	if ( sig.label.contains("ANNOTATIONS") ) {
        		annotations.push_back(ReadAnnotations( (char *)&signalPtr[pos], sig.nr*2));
        		pos += sig.nr * 2;
        	} else {	// it's got genuine 16-bit values
#ifdef Q_LITTLE_ENDIAN
    	        // Intel x86, etc..
    	        memcpy((char *)&sig.value[sig.pos], (char *)&signalPtr[pos], sig.nr * 2);
    	        sig.pos += sig.nr;
    	        pos += sig.nr * 2;
#else
    	        // Big endian safe
    	        for (int j=0;j<sig.nr;j++) {
    	            qint16 t=Read16();
    	            sig.value[sig.pos++]=t;
	            }
#endif
			}
        }
    }
	return true;
}

// Parse the EDF file to get the annotations out of it.
QVector<Annotation> * EDFInfo::ReadAnnotations(const char * data, int charLen)
{
	QVector<Annotation> * annoVec = new QVector<Annotation>;
	
    // Process event annotation record

    long pos = 0;
    double offset;
    double duration;

    while (pos < charLen) {
	    QString text;
        bool sign, ok;
        char c = data[pos];

        if ((c != '+') && (c != '-'))       // Annotaion must start with a +/- sign
            break;
        sign = (data[pos++] == '+');

        text = "";
        c = data[pos];

        do {            // collect the offset 
            text += c;
            pos++;
            c = data[pos];
        } while ((c != AnnoSep) && (c != AnnoDurMark)); // a duration is optional

        offset = text.toDouble(&ok);
        if (!ok) {
            qDebug() << "Faulty offset in  annotation record ";
        //  sleep(1);
            break;
        }

        if (!sign)
            offset = -offset;

        duration = -1.0;	// This indicates no duration supplied
        // First entry
        if (data[pos] == AnnoDurMark) { // get duration.(preceded by decimal 21 byte)
            pos++;
            text = "";

            do {        // collect the duration
                text += data[pos];
                pos++;
            } while ((data[pos] != AnnoSep) && (pos < charLen)); // separator code

            duration = text.toDouble(&ok);
            if (!ok) {
                qDebug() << "Faulty duration in  annotation record ";
        //      sleep(1);
                break;
            }
        }

        while ((data[pos] == AnnoSep) && (pos < charLen)) {
            int textLen = 0;
            pos++;
            const char * textStart = &data[pos];
            if (data[pos] == AnnoEnd)
                break;
            if (data[pos] == AnnoSep) {
                pos++;
                break;
            }
            do {            // collect the annotation text
                pos++;      // officially UTF-8 is allowed here, so don't mangle it
                textLen++;
            } while ((data[pos] != AnnoSep) && (pos < charLen)); // separator code
            text.fromUtf8(textStart, textLen);
            annoVec->push_back( Annotation( offset, duration, text) );
            if (pos >= charLen) {
                qDebug() << "Short EDF Annotations record";
        //      sleep(1);
                break;
            }
        }

        while ((pos < charLen) && (data[pos] == AnnoEnd))
            pos++;

        if (pos >= charLen)
            break;
    }
    return annoVec;
}

// Read a 16 bits integer
qint16 EDFInfo::Read16()
{
    if ((pos + 2) > datasize) {
        eof = true;
        return 0;
    }
#ifdef Q_LITTLE_ENDIAN // Intel, etc...
    qint16 res = *(qint16 *)&signalPtr[pos];
#else // ARM, PPC, etc..
    qint16 res = quint8(signalPtr[pos]) | (qint8(signalPtr[pos+1]) << 8);
#endif
    pos += 2;
    return res;
}

QString EDFInfo::ReadBytes(unsigned n)
{
    if ((pos + long(n)) > datasize) {
        eof = true;
        return QString();
    }
    QByteArray buf(&signalPtr[pos], n);
    pos+=n;
    return buf.trimmed();
}

EDFSignal *EDFInfo::lookupLabel(const QString & name, int index)
{
    auto it = signalList.find(name);
    if (it == signalList.end()) 
        return nullptr;

    if (index >= it.value().size()) 
        return nullptr;

    return it.value()[index];
}

