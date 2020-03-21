/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#define _DEFAULT_SOURCE
#include "os.h"

#include "mgmtDb.h"
#include "mgmtAcct.h"
#include "mgmtBalance.h"
#include "mgmtChildTable.h"
#include "mgmtDnode.h"
#include "mgmtGrant.h"
#include "mgmtMnode.h"
#include "mgmtNormalTable.h"
#include "mgmtShell.h"
#include "mgmtSuperTable.h"
#include "mgmtTable.h"
#include "mgmtUser.h"
#include "mgmtVgroup.h"
#include "mnode.h"

#include "taoserror.h"
#include "tstatus.h"
#include "tutil.h"
#include "name.h"

static void   *tsDbSdb = NULL;
static int32_t tsDbUpdateSize;

static int32_t mgmtCreateDb(SAcctObj *pAcct, SCMCreateDbMsg *pCreate);
static void    mgmtDropDb(void *handle, void *tmrId);
static void    mgmtSetDbDirty(SDbObj *pDb);

static int32_t mgmtGetDbMeta(STableMetaMsg *pMeta, SShowObj *pShow, void *pConn);
static int32_t mgmtRetrieveDbs(SShowObj *pShow, char *data, int32_t rows, void *pConn);
static void    mgmtProcessCreateDbMsg(SQueuedMsg *pMsg);
static void    mgmtProcessAlterDbMsg(SQueuedMsg *pMsg);
static void    mgmtProcessDropDbMsg(SQueuedMsg *pMsg);

static void *(*mgmtDbActionFp[SDB_MAX_ACTION_TYPES])(void *row, char *str, int32_t size, int32_t *ssize);
static void *mgmtDbActionInsert(void *row, char *str, int32_t size, int32_t *ssize);
static void *mgmtDbActionDelete(void *row, char *str, int32_t size, int32_t *ssize);
static void *mgmtDbActionUpdate(void *row, char *str, int32_t size, int32_t *ssize);
static void *mgmtDbActionEncode(void *row, char *str, int32_t size, int32_t *ssize);
static void *mgmtDbActionDecode(void *row, char *str, int32_t size, int32_t *ssize);
static void *mgmtDbActionReset(void *row, char *str, int32_t size, int32_t *ssize);
static void *mgmtDbActionDestroy(void *row, char *str, int32_t size, int32_t *ssize);

static void mgmtDbActionInit() {
  mgmtDbActionFp[SDB_TYPE_INSERT] = mgmtDbActionInsert;
  mgmtDbActionFp[SDB_TYPE_DELETE] = mgmtDbActionDelete;
  mgmtDbActionFp[SDB_TYPE_UPDATE] = mgmtDbActionUpdate;
  mgmtDbActionFp[SDB_TYPE_ENCODE] = mgmtDbActionEncode;
  mgmtDbActionFp[SDB_TYPE_DECODE] = mgmtDbActionDecode;
  mgmtDbActionFp[SDB_TYPE_RESET]  = mgmtDbActionReset;
  mgmtDbActionFp[SDB_TYPE_DESTROY] = mgmtDbActionDestroy;
}

static void *mgmtDbAction(char action, void *row, char *str, int32_t size, int32_t *ssize) {
  if (mgmtDbActionFp[(uint8_t)action] != NULL) {
    return (*(mgmtDbActionFp[(uint8_t)action]))(row, str, size, ssize);
  }
  return NULL;
}

int32_t mgmtInitDbs() {
  void *    pNode = NULL;
  SDbObj *  pDb = NULL;
  SAcctObj *pAcct = NULL;

  mgmtDbActionInit();

  SDbObj tObj;
  tsDbUpdateSize = tObj.updateEnd - (char *)&tObj;

  tsDbSdb = sdbOpenTable(tsMaxDbs, tsDbUpdateSize, "dbs", SDB_KEYTYPE_STRING, tsMnodeDir, mgmtDbAction);
  if (tsDbSdb == NULL) {
    mError("failed to init db data");
    return -1;
  }

  while (1) {
    pNode = sdbFetchRow(tsDbSdb, pNode, (void **)&pDb);
    if (pDb == NULL) break;

    pDb->pHead = NULL;
    pDb->pTail = NULL;
    pDb->prev = NULL;
    pDb->next = NULL;
    pDb->numOfTables = 0;
    pDb->numOfVgroups = 0;
    pDb->numOfSuperTables = 0;
    pAcct = mgmtGetAcct(pDb->cfg.acct);
    if (pAcct != NULL)
      mgmtAddDbIntoAcct(pAcct, pDb);
    else {
      mError("db:%s acct:%s info not exist in sdb", pDb->name, pDb->cfg.acct);
    }
  }

  mgmtAddShellMsgHandle(TSDB_MSG_TYPE_CM_CREATE_DB, mgmtProcessCreateDbMsg);
  mgmtAddShellMsgHandle(TSDB_MSG_TYPE_CM_ALTER_DB, mgmtProcessAlterDbMsg);
  mgmtAddShellMsgHandle(TSDB_MSG_TYPE_CM_DROP_DB, mgmtProcessDropDbMsg);
  mgmtAddShellShowMetaHandle(TSDB_MGMT_TABLE_DB, mgmtGetDbMeta);
  mgmtAddShellShowRetrieveHandle(TSDB_MGMT_TABLE_DB, mgmtRetrieveDbs);

  mTrace("db data is initialized");
  return 0;
}

SDbObj *mgmtGetDb(char *db) {
  return (SDbObj *)sdbGetRow(tsDbSdb, db);
}

SDbObj *mgmtGetDbByTableId(char *tableId) {
  char db[TSDB_TABLE_ID_LEN], *pos;

  pos = strstr(tableId, TS_PATH_DELIMITER);
  pos = strstr(pos + 1, TS_PATH_DELIMITER);
  memset(db, 0, sizeof(db));
  strncpy(db, tableId, pos - tableId);

  return (SDbObj *)sdbGetRow(tsDbSdb, db);
}

static int32_t mgmtCheckDBParams(SCMCreateDbMsg *pCreate) {
  if (pCreate->commitLog < 0 || pCreate->commitLog > 1) {
    mError("invalid db option commitLog: %d, only 0 or 1 allowed", pCreate->commitLog);
    return TSDB_CODE_INVALID_OPTION;
  }

  if (pCreate->replications < TSDB_REPLICA_MIN_NUM || pCreate->replications > TSDB_REPLICA_MAX_NUM) {
    mError("invalid db option replications: %d valid range: [%d, %d]", pCreate->replications, TSDB_REPLICA_MIN_NUM,
           TSDB_REPLICA_MAX_NUM);
    return TSDB_CODE_INVALID_OPTION;
  }

  if (pCreate->daysPerFile < TSDB_FILE_MIN_PARTITION_RANGE || pCreate->daysPerFile > TSDB_FILE_MAX_PARTITION_RANGE) {
    mError("invalid db option daysPerFile: %d valid range: [%d, %d]", pCreate->daysPerFile, TSDB_FILE_MIN_PARTITION_RANGE,
           TSDB_FILE_MAX_PARTITION_RANGE);
    return TSDB_CODE_INVALID_OPTION;
  }

  if (pCreate->daysToKeep1 > pCreate->daysToKeep2 || pCreate->daysToKeep2 > pCreate->daysToKeep) {
    mError("invalid db option daystokeep1: %d, daystokeep2: %d, daystokeep: %d", pCreate->daysToKeep1,
           pCreate->daysToKeep2, pCreate->daysToKeep);
    return TSDB_CODE_INVALID_OPTION;
  }

  if (pCreate->daysToKeep1 < TSDB_FILE_MIN_PARTITION_RANGE || pCreate->daysToKeep1 < pCreate->daysPerFile) {
    mError("invalid db option daystokeep: %d", pCreate->daysToKeep);
    return TSDB_CODE_INVALID_OPTION;
  }

  if (pCreate->rowsInFileBlock < TSDB_MIN_ROWS_IN_FILEBLOCK || pCreate->rowsInFileBlock > TSDB_MAX_ROWS_IN_FILEBLOCK) {
    mError("invalid db option rowsInFileBlock: %d valid range: [%d, %d]", pCreate->rowsInFileBlock,
           TSDB_MIN_ROWS_IN_FILEBLOCK, TSDB_MAX_ROWS_IN_FILEBLOCK);
    return TSDB_CODE_INVALID_OPTION;
  }

  if (pCreate->cacheBlockSize < TSDB_MIN_CACHE_BLOCK_SIZE || pCreate->cacheBlockSize > TSDB_MAX_CACHE_BLOCK_SIZE) {
    mError("invalid db option cacheBlockSize: %d valid range: [%d, %d]", pCreate->cacheBlockSize,
           TSDB_MIN_CACHE_BLOCK_SIZE, TSDB_MAX_CACHE_BLOCK_SIZE);
    return TSDB_CODE_INVALID_OPTION;
  }

  if (pCreate->maxSessions < TSDB_MIN_TABLES_PER_VNODE || pCreate->maxSessions > TSDB_MAX_TABLES_PER_VNODE) {
    mError("invalid db option maxSessions: %d valid range: [%d, %d]", pCreate->maxSessions, TSDB_MIN_TABLES_PER_VNODE,
           TSDB_MAX_TABLES_PER_VNODE);
    return TSDB_CODE_INVALID_OPTION;
  }

  if (pCreate->precision != TSDB_TIME_PRECISION_MILLI && pCreate->precision != TSDB_TIME_PRECISION_MICRO) {
    mError("invalid db option timePrecision: %d valid value: [%d, %d]", pCreate->precision, TSDB_TIME_PRECISION_MILLI,
           TSDB_TIME_PRECISION_MICRO);
    return TSDB_CODE_INVALID_OPTION;
  }

  if (pCreate->cacheNumOfBlocks.fraction < TSDB_MIN_AVG_BLOCKS || pCreate->cacheNumOfBlocks.fraction > TSDB_MAX_AVG_BLOCKS) {
    mError("invalid db option ablocks: %f valid value: [%d, %d]", pCreate->cacheNumOfBlocks.fraction, 0, TSDB_MAX_AVG_BLOCKS);
    return TSDB_CODE_INVALID_OPTION;
  }

  if (pCreate->commitTime < TSDB_MIN_COMMIT_TIME_INTERVAL || pCreate->commitTime > TSDB_MAX_COMMIT_TIME_INTERVAL) {
    mError("invalid db option commitTime: %d valid range: [%d, %d]", pCreate->commitTime, TSDB_MIN_COMMIT_TIME_INTERVAL,
           TSDB_MAX_COMMIT_TIME_INTERVAL);
    return TSDB_CODE_INVALID_OPTION;
  }

  if (pCreate->compression < TSDB_MIN_COMPRESSION_LEVEL || pCreate->compression > TSDB_MAX_COMPRESSION_LEVEL) {
    mError("invalid db option compression: %d valid range: [%d, %d]", pCreate->compression, TSDB_MIN_COMPRESSION_LEVEL,
           TSDB_MAX_COMPRESSION_LEVEL);
    return TSDB_CODE_INVALID_OPTION;
  }

  return TSDB_CODE_SUCCESS;
}

static int32_t mgmtCheckDbParams(SCMCreateDbMsg *pCreate) {
  // assign default parameters
  if (pCreate->maxSessions < 0) pCreate->maxSessions = tsSessionsPerVnode;      //
  if (pCreate->cacheBlockSize < 0) pCreate->cacheBlockSize = tsCacheBlockSize;  //
  if (pCreate->daysPerFile < 0) pCreate->daysPerFile = tsDaysPerFile;           //
  if (pCreate->daysToKeep < 0) pCreate->daysToKeep = tsDaysToKeep;              //
  if (pCreate->daysToKeep1 < 0) pCreate->daysToKeep1 = pCreate->daysToKeep;     //
  if (pCreate->daysToKeep2 < 0) pCreate->daysToKeep2 = pCreate->daysToKeep;     //
  if (pCreate->commitTime < 0) pCreate->commitTime = tsCommitTime;              //
  if (pCreate->compression < 0) pCreate->compression = tsCompression;           //
  if (pCreate->commitLog < 0) pCreate->commitLog = tsCommitLog;
  if (pCreate->replications < 0) pCreate->replications = tsReplications;                                  //
  if (pCreate->rowsInFileBlock < 0) pCreate->rowsInFileBlock = tsRowsInFileBlock;                         //
  if (pCreate->cacheNumOfBlocks.fraction < 0) pCreate->cacheNumOfBlocks.fraction = tsAverageCacheBlocks;  //
  
  if (mgmtCheckDBParams(pCreate) != TSDB_CODE_SUCCESS) {
    return TSDB_CODE_INVALID_OPTION;
  }
  
  pCreate->cacheNumOfBlocks.totalBlocks = (int32_t)(pCreate->cacheNumOfBlocks.fraction * pCreate->maxSessions);
  
  if (pCreate->cacheNumOfBlocks.totalBlocks > TSDB_MAX_CACHE_BLOCKS) {
    mTrace("invalid db option cacheNumOfBlocks: %d valid range: [%d, %d]", pCreate->cacheNumOfBlocks.totalBlocks,
           TSDB_MIN_CACHE_BLOCKS, TSDB_MAX_CACHE_BLOCKS);
    return TSDB_CODE_INVALID_OPTION;
  }

  // calculate the blocks per table
  if (pCreate->blocksPerTable < 0) {
    pCreate->blocksPerTable = pCreate->cacheNumOfBlocks.totalBlocks / 4;
  }
  
  if (pCreate->blocksPerTable > pCreate->cacheNumOfBlocks.totalBlocks * 3 / 4) {
    pCreate->blocksPerTable = pCreate->cacheNumOfBlocks.totalBlocks * 3 / 4;
  }
  
  if (pCreate->blocksPerTable < TSDB_MIN_AVG_BLOCKS) {
    pCreate->blocksPerTable = TSDB_MIN_AVG_BLOCKS;
  }

  pCreate->maxSessions++;

  return TSDB_CODE_SUCCESS;
}

static int32_t mgmtCreateDb(SAcctObj *pAcct, SCMCreateDbMsg *pCreate) {
  int32_t numOfDbs = sdbGetNumOfRows(tsDbSdb);
  if (numOfDbs >= tsMaxDbs) {
    mWarn("numOfDbs:%d, exceed tsMaxDbs:%d", numOfDbs, tsMaxDbs);
    return TSDB_CODE_TOO_MANY_DATABASES;
  }

  int32_t code = mgmtCheckDbLimit(pAcct);
  if (code != 0) {
    return code;
  }

  SDbObj *pDb = (SDbObj *)sdbGetRow(tsDbSdb, pCreate->db);
  if (pDb != NULL) {
    return TSDB_CODE_DB_ALREADY_EXIST;
  }

  code = mgmtCheckDbParams(pCreate);
  if (code != TSDB_CODE_SUCCESS) return code;

  assert(pCreate->daysToKeep1 <= pCreate->daysToKeep2 && pCreate->daysToKeep2 <= pCreate->daysToKeep);

  code = mgmtCheckDbGrant();
  if (code != 0) {
    return code;
  }

  pDb = malloc(sizeof(SDbObj));
  memset(pDb, 0, sizeof(SDbObj));
  strcpy(pDb->name, pCreate->db);
  strcpy(pCreate->acct, pAcct->user);
  pDb->createdTime = taosGetTimestampMs();
  pDb->cfg = *pCreate;

  if (sdbInsertRow(tsDbSdb, pDb, 0) < 0) {
    code = TSDB_CODE_SDB_ERROR;
    tfree(pDb);
  }

  return code;
}

bool mgmtCheckIsMonitorDB(char *db, char *monitordb) {
  char dbName[TSDB_DB_NAME_LEN + 1] = {0};
  extractDBName(db, dbName);

  size_t len = strlen(dbName);
  return (strncasecmp(dbName, monitordb, len) == 0 && len == strlen(monitordb));
}

static int32_t mgmtAlterDb(SAcctObj *pAcct, SCMAlterDbMsg *pAlter) {
  return 0;
//  int32_t code = TSDB_CODE_SUCCESS;
//
//  SDbObj *pDb = (SDbObj *) sdbGetRow(tsDbSdb, pAlter->db);
//  if (pDb == NULL) {
//    mTrace("db:%s is not exist", pAlter->db);
//    return TSDB_CODE_INVALID_DB;
//  }
//
//  int32_t oldReplicaNum = pDb->cfg.replications;
//  if (pAlter->daysToKeep > 0) {
//    mTrace("db:%s daysToKeep:%d change to %d", pDb->name, pDb->cfg.daysToKeep, pAlter->daysToKeep);
//    pDb->cfg.daysToKeep = pAlter->daysToKeep;
//  } else if (pAlter->replications > 0) {
//    mTrace("db:%s replica:%d change to %d", pDb->name, pDb->cfg.replications, pAlter->replications);
//    if (pAlter->replications < TSDB_REPLICA_MIN_NUM || pAlter->replications > TSDB_REPLICA_MAX_NUM) {
//      mError("invalid db option replica: %d valid range: %d--%d", pAlter->replications, TSDB_REPLICA_MIN_NUM, TSDB_REPLICA_MAX_NUM);
//      return TSDB_CODE_INVALID_OPTION;
//    }
//    pDb->cfg.replications = pAlter->replications;
//  } else if (pAlter->maxSessions > 0) {
//    mTrace("db:%s tables:%d change to %d", pDb->name, pDb->cfg.maxSessions, pAlter->maxSessions);
//    if (pAlter->maxSessions < TSDB_MIN_TABLES_PER_VNODE || pAlter->maxSessions > TSDB_MAX_TABLES_PER_VNODE) {
//      mError("invalid db option tables: %d valid range: %d--%d", pAlter->maxSessions, TSDB_MIN_TABLES_PER_VNODE, TSDB_MAX_TABLES_PER_VNODE);
//      return TSDB_CODE_INVALID_OPTION;
//    }
//    if (pAlter->maxSessions < pDb->cfg.maxSessions) {
//      mError("invalid db option tables: %d should larger than original:%d", pAlter->maxSessions, pDb->cfg.maxSessions);
//      return TSDB_CODE_INVALID_OPTION;
//    }
//    return TSDB_CODE_INVALID_OPTION;
//    //The modification of tables needs to rewrite the head file, so disable this option
//    //pDb->cfg.maxSessions = pAlter->maxSessions;
//  } else {
//    mError("db:%s alter msg, replica:%d, keep:%d, tables:%d, origin replica:%d keep:%d", pDb->name,
//            pAlter->replications, pAlter->maxSessions, pAlter->daysToKeep,
//            pDb->cfg.replications, pDb->cfg.daysToKeep);
//    return TSDB_CODE_INVALID_OPTION;
//  }
//
//  if (sdbUpdateRow(tsDbSdb, pDb, tsDbUpdateSize, 1) < 0) {
//    return TSDB_CODE_SDB_ERROR;
//  }
//
//  SVgObj *pVgroup = pDb->pHead;
//  while (pVgroup != NULL) {
//    balanceUpdateVgroupState(pVgroup, TSDB_VG_LB_STATUS_UPDATE, 0);
//    if (oldReplicaNum < pDb->cfg.replications) {
//      if (!balanceAddVnode(pVgroup, NULL, NULL)) {
//        mWarn("db:%s vgroup:%d not enough dnode to add vnode", pAlter->db, pVgroup->vgId);
//        code = TSDB_CODE_NO_ENOUGH_DNODES;
//      }
//    }
//    if (pAlter->maxSessions > 0) {
//      //rebuild meterList in mgmtVgroup.c
//      mgmtUpdateVgroup(pVgroup);
//    }
////    mgmtSendCreateVnodeMsg(pVgroup);
//    pVgroup = pVgroup->next;
//  }
//  mgmtStartBalanceTimer(10);
//
//  return code;
}

int32_t mgmtAddVgroupIntoDb(SDbObj *pDb, SVgObj *pVgroup) {
  pVgroup->next = pDb->pHead;
  pVgroup->prev = NULL;

  if (pDb->pHead) pDb->pHead->prev = pVgroup;
  if (pDb->pTail == NULL) pDb->pTail = pVgroup;

  pDb->pHead = pVgroup;
  pDb->numOfVgroups++;

  return 0;
}

int32_t mgmtAddVgroupIntoDbTail(SDbObj *pDb, SVgObj *pVgroup) {
  pVgroup->next = NULL;
  pVgroup->prev = pDb->pTail;

  if (pDb->pTail) pDb->pTail->next = pVgroup;
  if (pDb->pHead == NULL) pDb->pHead = pVgroup;

  pDb->pTail = pVgroup;
  pDb->numOfVgroups++;

  return 0;
}

int32_t mgmtRemoveVgroupFromDb(SDbObj *pDb, SVgObj *pVgroup) {
  if (pVgroup->prev) pVgroup->prev->next = pVgroup->next;
  if (pVgroup->next) pVgroup->next->prev = pVgroup->prev;
  if (pVgroup->prev == NULL) pDb->pHead = pVgroup->next;
  if (pVgroup->next == NULL) pDb->pTail = pVgroup->prev;
  pDb->numOfVgroups--;

  return 0;
}

int32_t mgmtMoveVgroupToTail(SDbObj *pDb, SVgObj *pVgroup) {
  mgmtRemoveVgroupFromDb(pDb, pVgroup);
  mgmtAddVgroupIntoDbTail(pDb, pVgroup);

  return 0;
}

int32_t mgmtMoveVgroupToHead(SDbObj *pDb, SVgObj *pVgroup) {
  mgmtRemoveVgroupFromDb(pDb, pVgroup);
  mgmtAddVgroupIntoDb(pDb, pVgroup);

  return 0;
}

void mgmtCleanUpDbs() {
  sdbCloseTable(tsDbSdb);
}

static int32_t mgmtGetDbMeta(STableMetaMsg *pMeta, SShowObj *pShow, void *pConn) {
  int32_t cols = 0;

  SSchema *pSchema = pMeta->schema;
  SUserObj *pUser = mgmtGetUserFromConn(pConn);
  if (pUser == NULL) return 0;

  pShow->bytes[cols] = TSDB_DB_NAME_LEN;
  pSchema[cols].type = TSDB_DATA_TYPE_BINARY;
  strcpy(pSchema[cols].name, "name");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = 8;
  pSchema[cols].type = TSDB_DATA_TYPE_TIMESTAMP;
  strcpy(pSchema[cols].name, "created_time");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = 4;
  pSchema[cols].type = TSDB_DATA_TYPE_INT;
  strcpy(pSchema[cols].name, "ntables");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

#ifndef __CLOUD_VERSION__
  if (strcmp(pUser->user, "root") == 0) {
#endif
    pShow->bytes[cols] = 4;
    pSchema[cols].type = TSDB_DATA_TYPE_INT;
    strcpy(pSchema[cols].name, "vgroups");
    pSchema[cols].bytes = htons(pShow->bytes[cols]);
    cols++;
#ifndef __CLOUD_VERSION__
  }
#endif

#ifndef __CLOUD_VERSION__
  if (strcmp(pUser->user, "root") == 0) {
#endif
    pShow->bytes[cols] = 2;
    pSchema[cols].type = TSDB_DATA_TYPE_SMALLINT;
    strcpy(pSchema[cols].name, "replica");
    pSchema[cols].bytes = htons(pShow->bytes[cols]);
    cols++;

    pShow->bytes[cols] = 2;
    pSchema[cols].type = TSDB_DATA_TYPE_SMALLINT;
    strcpy(pSchema[cols].name, "days");
    pSchema[cols].bytes = htons(pShow->bytes[cols]);
    cols++;
#ifndef __CLOUD_VERSION__
  }
#endif

  pShow->bytes[cols] = 24;
  pSchema[cols].type = TSDB_DATA_TYPE_BINARY;
  strcpy(pSchema[cols].name, "keep1,keep2,keep(D)");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

#ifndef __CLOUD_VERSION__
  if (strcmp(pUser->user, "root") == 0) {
#endif
    pShow->bytes[cols] = 4;
    pSchema[cols].type = TSDB_DATA_TYPE_INT;
    strcpy(pSchema[cols].name, "tables");
    pSchema[cols].bytes = htons(pShow->bytes[cols]);
    cols++;

    pShow->bytes[cols] = 4;
    pSchema[cols].type = TSDB_DATA_TYPE_INT;
    strcpy(pSchema[cols].name, "rows");
    pSchema[cols].bytes = htons(pShow->bytes[cols]);
    cols++;

    pShow->bytes[cols] = 4;
    pSchema[cols].type = TSDB_DATA_TYPE_INT;
    strcpy(pSchema[cols].name, "cache(b)");
    pSchema[cols].bytes = htons(pShow->bytes[cols]);
    cols++;

    pShow->bytes[cols] = 4;
    pSchema[cols].type = TSDB_DATA_TYPE_FLOAT;
    strcpy(pSchema[cols].name, "ablocks");
    pSchema[cols].bytes = htons(pShow->bytes[cols]);
    cols++;

    pShow->bytes[cols] = 2;
    pSchema[cols].type = TSDB_DATA_TYPE_SMALLINT;
    strcpy(pSchema[cols].name, "tblocks");
    pSchema[cols].bytes = htons(pShow->bytes[cols]);
    cols++;

    pShow->bytes[cols] = 4;
    pSchema[cols].type = TSDB_DATA_TYPE_INT;
    strcpy(pSchema[cols].name, "ctime(s)");
    pSchema[cols].bytes = htons(pShow->bytes[cols]);
    cols++;

    pShow->bytes[cols] = 1;
    pSchema[cols].type = TSDB_DATA_TYPE_TINYINT;
    strcpy(pSchema[cols].name, "clog");
    pSchema[cols].bytes = htons(pShow->bytes[cols]);
    cols++;

    pShow->bytes[cols] = 1;
    pSchema[cols].type = TSDB_DATA_TYPE_TINYINT;
    strcpy(pSchema[cols].name, "comp");
    pSchema[cols].bytes = htons(pShow->bytes[cols]);
    cols++;
#ifndef __CLOUD_VERSION__
  }
#endif

  pShow->bytes[cols] = 3;
  pSchema[cols].type = TSDB_DATA_TYPE_BINARY;
  strcpy(pSchema[cols].name, "time precision");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = 10;
  pSchema[cols].type = TSDB_DATA_TYPE_BINARY;
  strcpy(pSchema[cols].name, "status");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pMeta->numOfColumns = htons(cols);
  pShow->numOfColumns = cols;

  pShow->offset[0] = 0;
  for (int32_t i = 1; i < cols; ++i) {
    pShow->offset[i] = pShow->offset[i - 1] + pShow->bytes[i - 1];
  }

  pShow->rowSize = pShow->offset[cols - 1] + pShow->bytes[cols - 1];

  pShow->numOfRows = pUser->pAcct->acctInfo.numOfDbs;
  pShow->pNode = pUser->pAcct->pHead;

  return 0;
}

char *mgmtGetDbStr(char *src) {
  char *pos = strstr(src, TS_PATH_DELIMITER);

  return ++pos;
}

static int32_t mgmtRetrieveDbs(SShowObj *pShow, char *data, int32_t rows, void *pConn) {
  int32_t numOfRows = 0;
  SDbObj *pDb = NULL;
  char *  pWrite;
  int32_t cols = 0;
  SUserObj *pUser = mgmtGetUserFromConn(pConn);
  if (pUser == NULL) return 0;

  while (numOfRows < rows) {
    pDb = (SDbObj *)pShow->pNode;
    if (pDb == NULL) break;
    pShow->pNode = (void *)pDb->next;
    if (mgmtCheckIsMonitorDB(pDb->name, tsMonitorDbName)) {
      if (strcmp(pUser->user, "root") != 0 && strcmp(pUser->user, "_root") != 0 && strcmp(pUser->user, "monitor") != 0 ) {
        continue;
      }
    }

    cols = 0;

    pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
    strcpy(pWrite, mgmtGetDbStr(pDb->name));
    cols++;

    pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
    *(int64_t *)pWrite = pDb->createdTime;
    cols++;

    pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
    *(int32_t *)pWrite = pDb->numOfTables;
    cols++;

#ifndef __CLOUD_VERSION__
    if (strcmp(pUser->user, "root") == 0) {
#endif
      pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
      *(int32_t *)pWrite = pDb->numOfVgroups;
      cols++;
#ifndef __CLOUD_VERSION__
    }
#endif

#ifndef __CLOUD_VERSION__
    if (strcmp(pUser->user, "root") == 0) {
#endif
      pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
      *(int16_t *)pWrite = pDb->cfg.replications;
      cols++;

      pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
      *(int16_t *)pWrite = pDb->cfg.daysPerFile;
      cols++;
#ifndef __CLOUD_VERSION__
    }
#endif

    pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
    sprintf(pWrite, "%d,%d,%d", pDb->cfg.daysToKeep1, pDb->cfg.daysToKeep2, pDb->cfg.daysToKeep);
    cols++;

#ifndef __CLOUD_VERSION__
    if (strcmp(pUser->user, "root") == 0) {
#endif
      pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
      *(int32_t *)pWrite = pDb->cfg.maxSessions - 1;  // table num can be created should minus 1
      cols++;

      pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
      *(int32_t *)pWrite = pDb->cfg.rowsInFileBlock;
      cols++;

      pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
      *(int32_t *)pWrite = pDb->cfg.cacheBlockSize;
      cols++;

      pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
#ifdef _TD_ARM_32_
      *(int32_t *)pWrite = (pDb->cfg.cacheNumOfBlocks.totalBlocks * 1.0 / (pDb->cfg.maxSessions - 1));
#else
      *(float *)pWrite = (pDb->cfg.cacheNumOfBlocks.totalBlocks * 1.0 / (pDb->cfg.maxSessions - 1));
#endif
      cols++;

      pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
      *(int16_t *)pWrite = pDb->cfg.blocksPerTable;
      cols++;

      pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
      *(int32_t *)pWrite = pDb->cfg.commitTime;
      cols++;

      pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
      *(int8_t *)pWrite = pDb->cfg.commitLog;
      cols++;

      pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
      *(int8_t *)pWrite = pDb->cfg.compression;
      cols++;
#ifndef __CLOUD_VERSION__
    }
#endif

    pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
    char *prec = (pDb->cfg.precision == TSDB_TIME_PRECISION_MILLI) ? TSDB_TIME_PRECISION_MILLI_STR
                                                                   : TSDB_TIME_PRECISION_MICRO_STR;
    strcpy(pWrite, prec);
    cols++;

    pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
    strcpy(pWrite, pDb->dirty != TSDB_DB_STATUS_READY ? "dropping" : "ready");
    cols++;

    numOfRows++;
  }

  pShow->numOfReads += numOfRows;
  return numOfRows;
}

void *mgmtDbActionInsert(void *row, char *str, int32_t size, int32_t *ssize) {
  SDbObj *pDb = (SDbObj *) row;
  SAcctObj *pAcct = mgmtGetAcct(pDb->cfg.acct);

  pDb->pHead = NULL;
  pDb->pTail = NULL;
  pDb->numOfVgroups = 0;
  pDb->numOfTables = 0;
  mgmtAddDbIntoAcct(pAcct, pDb);

  return NULL;
}

void *mgmtDbActionDelete(void *row, char *str, int32_t size, int32_t *ssize) {
  SDbObj *pDb = (SDbObj *) row;
  SAcctObj *pAcct = mgmtGetAcct(pDb->cfg.acct);
  mgmtRemoveDbFromAcct(pAcct, pDb);

  mgmtDropAllNormalTables(pDb);
  mgmtDropAllChildTables(pDb);
  mgmtDropAllSuperTables(pDb);

  return NULL;
}

void *mgmtDbActionUpdate(void *row, char *str, int32_t size, int32_t *ssize) {
  return mgmtDbActionReset(row, str, size, ssize);
}

void *mgmtDbActionEncode(void *row, char *str, int32_t size, int32_t *ssize) {
  SDbObj  *pDb  = (SDbObj *) row;
  if (size < tsDbUpdateSize) {
    *ssize = -1;
  } else {
    memcpy(str, pDb, tsDbUpdateSize);
    *ssize = tsDbUpdateSize;
  }

  return NULL;
}
void *mgmtDbActionDecode(void *row, char *str, int32_t size, int32_t *ssize) {
  SDbObj *pDb = (SDbObj *) malloc(sizeof(SDbObj));
  if (pDb == NULL) return NULL;
  memset(pDb, 0, sizeof(SDbObj));

  memcpy(pDb, str, tsDbUpdateSize);

  return (void *)pDb;
}

void *mgmtDbActionReset(void *row, char *str, int32_t size, int32_t *ssize) {
  SDbObj  *pDb  = (SDbObj *) row;
  memcpy(pDb, str, tsDbUpdateSize);

  return NULL;
}

void *mgmtDbActionDestroy(void *row, char *str, int32_t size, int32_t *ssize) {
  tfree(row);
  return NULL;
}

void mgmtAddSuperTableIntoDb(SDbObj *pDb) {
  atomic_add_fetch_32(&pDb->numOfSuperTables, 1);
}

void mgmtRemoveSuperTableFromDb(SDbObj *pDb) {
  atomic_add_fetch_32(&pDb->numOfSuperTables, -1);
}
void mgmtAddTableIntoDb(SDbObj *pDb) {
  atomic_add_fetch_32(&pDb->numOfTables, 1);
}

void mgmtRemoveTableFromDb(SDbObj *pDb) {
  atomic_add_fetch_32(&pDb->numOfTables, -1);
}

static void mgmtSetDbDirty(SDbObj *pDb) {
  pDb->dirty = true;
}

static void mgmtProcessCreateDbMsg(SQueuedMsg *pMsg) {
  if (mgmtCheckRedirect(pMsg->thandle)) return;

  SCMCreateDbMsg *pCreate = pMsg->pCont;
  pCreate->maxSessions     = htonl(pCreate->maxSessions);
  pCreate->cacheBlockSize  = htonl(pCreate->cacheBlockSize);
  pCreate->daysPerFile     = htonl(pCreate->daysPerFile);
  pCreate->daysToKeep      = htonl(pCreate->daysToKeep);
  pCreate->daysToKeep1     = htonl(pCreate->daysToKeep1);
  pCreate->daysToKeep2     = htonl(pCreate->daysToKeep2);
  pCreate->commitTime      = htonl(pCreate->commitTime);
  pCreate->blocksPerTable  = htons(pCreate->blocksPerTable);
  pCreate->rowsInFileBlock = htonl(pCreate->rowsInFileBlock);

  int32_t code;
  if (mgmtCheckExpired()) {
    code = TSDB_CODE_GRANT_EXPIRED;
  } else if (!pMsg->pUser->writeAuth) {
    code = TSDB_CODE_NO_RIGHTS;
  } else {
    code = mgmtCreateDb(pMsg->pUser->pAcct, pCreate);
    if (code == TSDB_CODE_SUCCESS) {
      mLPrint("DB:%s is created by %s", pCreate->db, pMsg->pUser->user);
    }
  }

  mgmtSendSimpleResp(pMsg->thandle, code);
}

static void mgmtProcessAlterDbMsg(SQueuedMsg *pMsg) {
  if (mgmtCheckRedirect(pMsg->thandle)) return;

  SCMAlterDbMsg *pAlter = pMsg->pCont;
  pAlter->daysPerFile = htonl(pAlter->daysPerFile);
  pAlter->daysToKeep  = htonl(pAlter->daysToKeep);
  pAlter->maxSessions = htonl(pAlter->maxSessions) + 1;

  int32_t code;
  if (!pMsg->pUser->writeAuth) {
    code = TSDB_CODE_NO_RIGHTS;
  } else {
    code = mgmtAlterDb(pMsg->pUser->pAcct, pAlter);
    if (code == TSDB_CODE_SUCCESS) {
      mLPrint("DB:%s is altered by %s", pAlter->db, pMsg->pUser->user);
    }
  }

  mgmtSendSimpleResp(pMsg->thandle, code);
}

static void mgmtDropDb(void *handle, void *tmrId) {
  SQueuedMsg *newMsg = handle;
  SDbObj *pDb = newMsg->ahandle;
  mPrint("db:%s, drop db from sdb", pDb->name);

  int32_t code = sdbDeleteRow(tsDbSdb, pDb);
  if (code != 0) {
    code = TSDB_CODE_SDB_ERROR;
  }

  mgmtSendSimpleResp(newMsg->thandle, code);
  rpcFreeCont(newMsg->pCont);
  free(newMsg);
}

static void mgmtProcessDropDbMsg(SQueuedMsg *pMsg) {
  if (mgmtCheckRedirect(pMsg->thandle)) return;

  SCMDropDbMsg *pDrop = pMsg->pCont;
  mTrace("db:%s, drop db msg is received from thandle:%p", pDrop->db, pMsg->thandle);

  if (mgmtCheckExpired()) {
    mError("db:%s, failed to drop, grant expired", pDrop->db);
    mgmtSendSimpleResp(pMsg->thandle, TSDB_CODE_GRANT_EXPIRED);
    return;
  }

  if (!pMsg->pUser->writeAuth) {
    mError("db:%s, failed to drop, no rights", pDrop->db);
    mgmtSendSimpleResp(pMsg->thandle, TSDB_CODE_NO_RIGHTS);
    return;
  }

  SDbObj *pDb = mgmtGetDb(pDrop->db);
  if (pDb == NULL) {
    if (pDrop->ignoreNotExists) {
      mTrace("db:%s, db is not exist, think drop success", pDrop->db);
      mgmtSendSimpleResp(pMsg->thandle, TSDB_CODE_SUCCESS);
      return;
    } else {
      mError("db:%s, failed to drop, invalid db", pDrop->db);
      mgmtSendSimpleResp(pMsg->thandle, TSDB_CODE_INVALID_DB);
      return;
    }
  }

  if (mgmtCheckIsMonitorDB(pDb->name, tsMonitorDbName)) {
    mError("db:%s, can't drop monitor database", pDrop->db);
    mgmtSendSimpleResp(pMsg->thandle, TSDB_CODE_MONITOR_DB_FORBIDDEN);
    return;
  }

  mgmtSetDbDirty(pDb);

  SQueuedMsg *newMsg = malloc(sizeof(SQueuedMsg));
  memcpy(newMsg, pMsg, sizeof(SQueuedMsg));
  pMsg->pCont = NULL;

  SVgObj *pVgroup = pDb->pHead;
  if (pVgroup != NULL) {
    mPrint("vgroup:%d, will be dropped", pVgroup->vgId);
    newMsg->ahandle = pVgroup;
    newMsg->expected = pVgroup->numOfVnodes;
    mgmtDropVgroup(pVgroup, newMsg);
    return;
  }

  mTrace("db:%s, all vgroups is dropped", pDb->name);

  void *tmpTmr;
  newMsg->ahandle = pDb;
  taosTmrReset(mgmtDropDb, 10, newMsg, tsMgmtTmr, &tmpTmr);
}