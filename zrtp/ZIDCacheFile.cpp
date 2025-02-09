/*
 * Copyright 2006 - 2018, Werner Dittmann
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Authors: Werner Dittmann <Werner.Dittmann@t-online.de>
 */
// #define UNIT_TEST

#include <string>
#include <stdlib.h>

#ifdef _MSC_VER
#include <io.h>
#else
#include <unistd.h>
#endif

#include <crypto/zrtpDH.h>

#include <libzrtpcpp/ZIDCacheFile.h>

static int errors = 0;  // maybe we will use as member of ZIDCache later...


/**
 * Allow more than one cache file to exist for when
 * we test multiple users in the same process
 */
ZIDCache* getZidCacheInstance() {
    return new ZIDCacheFile();
}


void ZIDCacheFile::createZIDFile(char* name) {
    zidFile = fopen(name, "wb+");
    // New file, generate an associated random ZID and save
    // it as first record
    if (zidFile != NULL) {
        randomZRTP(associatedZid, IDENTIFIER_LEN);

        ZIDRecordFile rec;
        rec.setZid(associatedZid);
        rec.setOwnZIDRecord();
        fseek(zidFile, 0L, SEEK_SET);
        if (fwrite(rec.getRecordData(), rec.getRecordLength(), 1, zidFile) < 1)
            ++errors;
        fflush(zidFile);
    }
}

/**
 * Migrate old ZID file format to new one.
 *
 * If ZID file is old format:
 * - close it, rename it, then re-open
 * - create ZID file for new format
 * - copy over contents and flags.
 */
void ZIDCacheFile::checkDoMigration(char* name) {
    FILE* fdOld;
    unsigned char inb[2];
    zidrecord1_t recOld;

    fseek(zidFile, 0L, SEEK_SET);
    if (fread(inb, 2, 1, zidFile) < 1) {
        ++errors;
        inb[0] = 0;
    }

    if (inb[0] > 0) {           // if it's new format just return
        return;
    }
    fclose(zidFile);            // close old ZID file
    zidFile = NULL;

    // create save file name, rename and re-open
    // if rename fails, just unlink old ZID file and create a brand new file
    // just a little inconvenience for the user, need to verify new SAS
    std::string fn = std::string(name) + std::string(".save");
    if (rename(name, fn.c_str()) < 0) {
        unlink(name);
        createZIDFile(name);
        return;
    }
    fdOld = fopen(fn.c_str(), "rb");    // reopen old format in read only mode

    // Get first record from old file - is the own ZID
    fseek(fdOld, 0L, SEEK_SET);
    if (fread(&recOld, sizeof(zidrecord1_t), 1, fdOld) != 1) {
        fclose(fdOld);
        return;
    }
    if (recOld.ownZid != 1) {
        fclose(fdOld);
        return;
    }
    zidFile = fopen(name, "wb+");    // create new format file in binary r/w mode
    if (zidFile == NULL) {
        fclose(fdOld);
        return;
    }
    // create ZIDRecord in new format, copy over own ZID and write the record
    ZIDRecordFile rec;
    rec.setZid(recOld.identifier);
    rec.setOwnZIDRecord();
    if (fwrite(rec.getRecordData(), rec.getRecordLength(), 1, zidFile) < 1)
        ++errors;

    // now copy over all valid records from old ZID file format.
    // Sequentially read old records, sequentially write new records
    int numRead;
    do {
        numRead = fread(&recOld, sizeof(zidrecord1_t), 1, fdOld);
        if (numRead == 0) {     // all old records processed
            break;
        }
        // skip own ZID record and invalid records
        if (recOld.ownZid == 1 || recOld.recValid == 0) {
            continue;
        }
        ZIDRecordFile rec2;
        rec2.setZid(recOld.identifier);
        rec2.setValid();
        if (recOld.rs1Valid & SASVerified) {
            rec2.setSasVerified();
        }
        rec2.setNewRs1(recOld.rs2Data); // TODO: check squenec
        rec2.setNewRs1(recOld.rs1Data);
        if (fwrite(rec2.getRecordData(), rec2.getRecordLength(), 1, zidFile) < 1)
            ++errors;

    } while (numRead == 1);
    fclose(fdOld);
    fflush(zidFile);
}

ZIDCacheFile::~ZIDCacheFile() {
    close();
}

int ZIDCacheFile::open(char* name) {

    // check for an already active ZID file
    if (zidFile != NULL) {
        return 0;
    }
    if ((zidFile = fopen(name, "rb+")) == NULL) {
        createZIDFile(name);
    } else {
        checkDoMigration(name);
        if (zidFile != NULL) {
            ZIDRecordFile rec;
            fseek(zidFile, 0L, SEEK_SET);
            if (fread(rec.getRecordData(), rec.getRecordLength(), 1, zidFile) != 1) {
                fclose(zidFile);
                zidFile = NULL;
                return -1;
            }
            if (!rec.isOwnZIDRecord()) {
                fclose(zidFile);
                zidFile = NULL;
                return -1;
            }
            memcpy(associatedZid, rec.getIdentifier(), IDENTIFIER_LEN);
        }
    }
    return ((zidFile == NULL) ? -1 : 1);
}

void ZIDCacheFile::close() {

    if (zidFile != NULL) {
        fclose(zidFile);
        zidFile = NULL;
    }
}

ZIDRecord *ZIDCacheFile::getRecord(unsigned char *zid) {
    unsigned long pos;
    int numRead;
    //    ZIDRecordFile rec;
    ZIDRecordFile *zidRecord = new ZIDRecordFile();

    // set read pointer behind first record (
    fseek(zidFile, zidRecord->getRecordLength(), SEEK_SET);

    do {
        pos = ftell(zidFile);
        numRead = fread(zidRecord->getRecordData(), zidRecord->getRecordLength(), 1, zidFile);
        if (numRead == 0) {
            break;
        }

        // skip own ZID record and invalid records
        if (zidRecord->isOwnZIDRecord() || !zidRecord->isValid()) {
            continue;
        }

    } while (numRead == 1 &&
             memcmp(zidRecord->getIdentifier(), zid, IDENTIFIER_LEN) != 0);

    // If we reached end of file, then no record with the ZID
    // found. We need to create a new ZID record.
    if (numRead == 0) {
        // create new record
        delete(zidRecord);
        zidRecord = new ZIDRecordFile();
        zidRecord->setZid(zid);
        zidRecord->setValid();
        if (fwrite(zidRecord->getRecordData(), zidRecord->getRecordLength(), 1, zidFile) < 1)
            ++errors;
    }
    //  remember position of record in file for save operation
    zidRecord->setPosition(pos);
    return zidRecord;
}

unsigned int ZIDCacheFile::saveRecord(ZIDRecord *zidRec) {
    ZIDRecordFile *zidRecord = reinterpret_cast<ZIDRecordFile *>(zidRec);

    fseek(zidFile, zidRecord->getPosition(), SEEK_SET);
    if (fwrite(zidRecord->getRecordData(), zidRecord->getRecordLength(), 1, zidFile) < 1)
        ++errors;
    fflush(zidFile);
    return 1;
}

int32_t ZIDCacheFile::getPeerName(const uint8_t *peerZid, std::string *name) {
    return 0;
}

void ZIDCacheFile::putPeerName(const uint8_t *peerZid, const std::string name) {
    return;
}
