/* drvGsIP488.c */

/*************************************************************************
* Copyright (c) 2003 The University of Chicago, as Operator of Argonne
* National Laboratory.
* This code is distributed subject to a Software License Agreement
* found in file LICENSE that is included with this distribution.
*************************************************************************/

/* Author: Marty Kraimer */
/*   date: 08JUN2004 */
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <string.h>

#include <drvIpac.h>        /* IP management (from drvIpac) */

#include <epicsTypes.h>
#include <epicsEvent.h>
#include <epicsThread.h>
#include <epicsInterrupt.h>
#include <iocsh.h>
#include <callback.h>
#include <cantProceed.h>
#include <epicsTime.h>
#include <devLib.h>
#include <taskwd.h>
#include <asynDriver.h>
#include <asynGpibDriver.h>

#include <epicsExport.h>

int gsip488Debug = 0;
#define ERROR_MESSAGE_BUFFER_SIZE 160

#define GREEN_SPRING_ID  0xf0
#define GSIP_488  0x14

typedef enum {
    transferStateIdle, transferStateRead, transferStateWrite, transferStateCmd
} transferState_t;

typedef struct gsport {
    char        *portName;
    void        *asynGpibPvt;
    epicsUInt32 base;
    volatile epicsUInt8 *registers;
    int         vector;
    int         carrier;   /* Which IP carrier board*/
    int         module;    /* module number on carrier*/
    CALLBACK    callback;
    epicsUInt8  isr0; 
    epicsUInt8  isr1;
    epicsBoolean srqHappened;
    epicsBoolean srqEnabled;
    transferState_t transferState;
    transferState_t nextTransferState;
    /*bytesRemaining and nextByte are used by interruptHandler*/
    int         bytesRemainingCmd;
    epicsUInt8  *nextByteCmd;
    int         bytesRemainingWrite;
    epicsUInt8  *nextByteWrite;
    int         bytesRemainingRead;
    epicsUInt8  *nextByteRead;
    int         eos; /* -1 means no end of string character*/
    asynStatus  status; /*status of ip transfer*/
    epicsEventId waitForInterrupt;
    char errorMessage[ERROR_MESSAGE_BUFFER_SIZE];
}gsport;

static epicsUInt8 readRegister(gsport *pgsport, int offset);
static void writeRegister(gsport *pgsport,int offset, epicsUInt8 value);
static void printStatus(gsport *pgsport,const char *source);
/* routine to wait for io completion*/
static void waitTimeout(gsport *pgsport,double seconds);
/* Interrupt Handlers */
static void srqCallback(CALLBACK *pcallback);
void gsip488(void *pvt);
/*Routines that with interrupt handler perform I/O*/
static asynStatus writeCmd(gsport *pgsport,const char *buf, int cnt,
    double timeout,transferState_t nextState);
static asynStatus writeAddr(gsport *pgsport,int talk, int listen,
    double timeout,transferState_t nextState);
static asynStatus writeGpib(gsport *pgsport,const char *buf, int cnt,
    int *actual, int addr, double timeout);
static asynStatus readGpib(gsport *pgsport,char *buf, int cnt, int *actual,
    int addr, double timeout);
/* asynGpibPort methods */
static void gpibPortReport(void *pdrvPvt,FILE *fd,int details);
static asynStatus gpibPortConnect(void *pdrvPvt,asynUser *pasynUser);
static asynStatus gpibPortDisconnect(void *pdrvPvt,asynUser *pasynUser);
static asynStatus gpibPortSetPortOptions(void *pdrvPvt,asynUser *pasynUser,
    const char *key, const char *val);
static asynStatus gpibPortGetPortOptions(void *pdrvPvt,asynUser *pasynUser,
    const char *key, char *val, int sizeval);
static asynStatus gpibPortRead(void *pdrvPvt,asynUser *pasynUser,
    char *data,int maxchars,int *nbytesTransfered);
static asynStatus gpibPortWrite(void *pdrvPvt,asynUser *pasynUser,
    const char *data,int numchars,int *nbytesTransfered);
static asynStatus gpibPortFlush(void *pdrvPvt,asynUser *pasynUser);
static asynStatus gpibPortSetEos(void *pdrvPvt,asynUser *pasynUser,
    const char *eos,int eoslen);
static asynStatus gpibPortGetEos(void *pdrvPvt,asynUser *pasynUser,
    char *eos, int eossize, int *eoslen);
static asynStatus gpibPortAddressedCmd(void *pdrvPvt,asynUser *pasynUser,
    const char *data, int length);
static asynStatus gpibPortUniversalCmd(void *pdrvPvt, asynUser *pasynUser, int cmd);
static asynStatus gpibPortIfc(void *pdrvPvt, asynUser *pasynUser);
static asynStatus gpibPortRen(void *pdrvPvt,asynUser *pasynUser, int onOff);
static int gpibPortSrqStatus(void *pdrvPvt);
static asynStatus gpibPortSrqEnable(void *pdrvPvt, int onOff);
static asynStatus gpibPortSerialPollBegin(void *pdrvPvt);
static int gpibPortSerialPoll(void *pdrvPvt, int addr, double timeout);
static asynStatus gpibPortSerialPollEnd(void *pdrvPvt);

static asynGpibPort gpibPort = {
    gpibPortReport,
    gpibPortConnect,
    gpibPortDisconnect,
    gpibPortSetPortOptions,
    gpibPortGetPortOptions,
    gpibPortRead,
    gpibPortWrite,
    gpibPortFlush,
    gpibPortSetEos,
    gpibPortGetEos,
    gpibPortAddressedCmd,
    gpibPortUniversalCmd,
    gpibPortIfc,
    gpibPortRen,
    gpibPortSrqStatus,
    gpibPortSrqEnable,
    gpibPortSerialPollBegin,
    gpibPortSerialPoll,
    gpibPortSerialPollEnd
};

/* Register definitions */
#define ISR0    0x01 /*Interrupt Status Register 0*/
#define IMR0    0x01 /*Interrupt   Mask Register 0*/
#define ISR1    0x03 /*Interrupt Status Register 1*/
#define IMR1    0x03 /*Interrupt   Mask Register 1*/
#define ASR     0x05 /*Address Status Register*/
#define BSR     0x07 /*Bus Status Register*/
#define AUXMR   0x07 /*Auxiliary Mode Register*/
#define ADR     0x09 /*Address Register*/
#define DIR     0x0F /*Data In Register*/
#define CDOR    0x0F /*Command/Data Out Register*/
#define VECTOR  0x13 /*Vector Register*/

/*Definitions for ISR0*/
#define BI      0x20 /*Byte In*/
#define BO      0x10 /*Byte Out*/
#define END     0x08 /*Last Byte*/

/*Definitions for ISR1*/
#define ERR     0x40 /*Error*/
#define MA      0x04 /*My Address*/
#define SRQ     0x02 /*SRQ*/

/*Definitions for AUXMR*/
#define SWRSTC  0x00 /*Software reset clear*/
#define SWRSTS  0x80 /*Software reset set*/
#define RHDF    0x02 /*Release Ready For Data Holdoff*/
#define HDFAS   0x83 /*Holdoff on all data set*/
#define FEOI    0x08 /*Send EOI with next byte*/
#define LONC    0x09 /*Listen Only Clear*/
#define LONS    0x89 /*Listen Only Set*/
#define TONC    0x0A /*Talk Only Clear*/
#define TONS    0x8A /*Talk Only Set*/
#define GTS     0x0B /*Go to Standby*/
#define TCA     0x0C /*Take Control Asynchronously*/
#define TCS     0x0D /*Take Control Synchronously*/
#define IFCS    0x8F /*Interface Clear Set*/
#define IFCC    0x0F /*Interface Clear Clear*/
#define REMC    0x10 /*Remote Enable Clear*/
#define REMS    0x90 /*Remote Enable Clear*/

/*Definitions for ASR*/
#define REM     0x80 /*Remote*/
#define ATN     0x20 /*ATN*/
#define LADS    0x04 /*Addressed to Listen*/
#define TADS    0x02 /*Addressed to Talk*/

static epicsUInt8 readRegister(gsport *pgsport, int offset)
{
    volatile epicsUInt8 *pregister = (epicsUInt8 *)
                             (((char *)pgsport->registers)+offset);
    epicsUInt8 value;
    
    value = *pregister;
    if(gsip488Debug) {
        char message[100];

        sprintf(message,"readRegister pregister %p offset %x value %2.2x\n",
            pregister,offset,value);
        if(epicsInterruptIsInterruptContext()) {
            epicsInterruptContextMessage(message);
        } else {
            printf("%s",message);
        }
    }
    return value;
}

static void writeRegister(gsport *pgsport,int offset, epicsUInt8 value)
{
    volatile epicsUInt8 *pregister = (epicsUInt8 *)
                             (((char *)pgsport->registers)+offset);

    if(gsip488Debug) {
        char message[100];

        sprintf(message,"writeRegister pregister %p offset %x value %2.2x\n",
            pregister,offset,value);
        if(epicsInterruptIsInterruptContext()) {
            epicsInterruptContextMessage(message);
        } else {
            printf("%s",message);
        }
    }
    *pregister = value;
}

static void printStatus(gsport *pgsport, const char *source)
{
    sprintf(pgsport->errorMessage, "%s "
        "isr0 %2.2x isr1 %2.2x ASR %2.2x BSR %2.2x\n",
        source, pgsport->isr0,pgsport->isr1,
        readRegister(pgsport,ASR),readRegister(pgsport,BSR));
}

static void waitTimeout(gsport *pgsport,double seconds)
{
    epicsEventWaitStatus status;
    transferState_t saveState;

    if(seconds<0.0) {
        status = epicsEventWait(pgsport->waitForInterrupt);
    } else {
        status = epicsEventWaitWithTimeout(pgsport->waitForInterrupt,seconds);
    }
    if(status==epicsEventWaitOK) return;
    saveState = pgsport->transferState;
    pgsport->transferState = transferStateIdle;
    switch(saveState) {
    case transferStateRead:
        pgsport->status=asynTimeout;
        printStatus(pgsport,"waitTimeout transferStateRead\n");
        break;
    case transferStateWrite:
        pgsport->status=asynTimeout;
        printStatus(pgsport,"waitTimeout transferStateWrite\n");
        break;
    case transferStateCmd:
        pgsport->status=asynTimeout;
        printStatus(pgsport,"waitTimeout transferStateCmd\n");
        break;
    default:
    }
}

static void srqCallback(CALLBACK *pcallback)
{
    gsport *pgsport;

    callbackGetUser(pgsport,pcallback);
    if(!pgsport->srqEnabled) return;
    pgsport->srqHappened = epicsTrue;
    pasynGpib->srqHappened(pgsport->asynGpibPvt);
}

void gsip488(void *pvt)
{
    gsport          *pgsport = (gsport *)pvt;
    transferState_t state = pgsport->transferState;
    epicsUInt8      isr0,isr1,octet;
    char            message[80];

    pgsport->isr0 = isr0 = readRegister(pgsport,ISR0);
    pgsport->isr1 = isr1 = readRegister(pgsport,ISR1);
    if(isr1&SRQ) callbackRequest(&pgsport->callback);
    if(isr1&ERR) {
        if(state!=transferStateIdle) {
            sprintf(pgsport->errorMessage,"\n%s interruptHandler ERR state %d\n",
                pgsport->portName,state);
            pgsport->status = asynError;
            epicsEventSignal(pgsport->waitForInterrupt);
        }
        return;
    }
    switch(state) {
    case transferStateCmd:
        if(!isr0&BO) return;
        if(pgsport->bytesRemainingCmd > 0) {
            octet = *pgsport->nextByteCmd;
            writeRegister(pgsport,CDOR,(epicsUInt8)octet);
            --(pgsport->bytesRemainingCmd); ++(pgsport->nextByteCmd);
            return;
        }
        pgsport->transferState = pgsport->nextTransferState;
        switch(pgsport->transferState) {
        case transferStateIdle:
            epicsEventSignal(pgsport->waitForInterrupt); return;
        case transferStateWrite:
            writeRegister(pgsport,AUXMR,TONS); break;
        case transferStateRead:
            writeRegister(pgsport,AUXMR,LONS); break;
        default:
        }
        writeRegister(pgsport,AUXMR,GTS);
        if(pgsport->transferState!=transferStateWrite) return;
    case transferStateWrite:
        if(!isr0&BO) return;
        if(pgsport->bytesRemainingWrite == 0) {
            pgsport->transferState = transferStateIdle;
            writeRegister(pgsport,AUXMR,TONC);
            epicsEventSignal(pgsport->waitForInterrupt);
            break ;
        }
        if(pgsport->bytesRemainingWrite==1) writeRegister(pgsport,AUXMR,FEOI);
        octet = *pgsport->nextByteWrite;
        writeRegister(pgsport,CDOR,(epicsUInt8)octet);
        --(pgsport->bytesRemainingWrite); ++(pgsport->nextByteWrite);
        break;
    case transferStateRead:
        if(!isr0&BI) break;
        octet = readRegister(pgsport,DIR);
        *pgsport->nextByteRead = octet;
        --(pgsport->bytesRemainingRead); ++(pgsport->nextByteRead);
        if(((pgsport->eos != -1 ) && (octet == pgsport->eos)) 
        || (END&isr0) || (pgsport->bytesRemainingRead == 0)) {
            pgsport->transferState = transferStateIdle;
            writeRegister(pgsport,AUXMR,LONC);
            epicsEventSignal(pgsport->waitForInterrupt);
        }
        writeRegister(pgsport,AUXMR,RHDF);
        break;
    case transferStateIdle: 
        if(readRegister(pgsport,ASR&ATN)) return;
        if(!isr0&BI) return;
        octet = readRegister(pgsport,DIR);
        sprintf(message,"%s gsip488 received %2.2x\n",
             pgsport->portName,octet);
        epicsInterruptContextMessage(message);
    }
}

static asynStatus writeCmd(gsport *pgsport,const char *buf, int cnt,
    double timeout,transferState_t nextState)
{
    epicsUInt8 isr0 = pgsport->isr0;

    writeRegister(pgsport,AUXMR,TCA);
    if(!isr0&BO) {
        printStatus(pgsport,"writeCmd !isr0&BO\n");
        return asynTimeout;
    }
    pgsport->bytesRemainingCmd = cnt-1;
    pgsport->nextByteCmd = (epicsUInt8 *)(buf+1);
    pgsport->transferState = transferStateCmd;
    pgsport->nextTransferState = nextState;
    pgsport->status = asynSuccess;
    writeRegister(pgsport,CDOR,(epicsUInt8)buf[0]);
    waitTimeout(pgsport,timeout);
    return pgsport->status;
}

static asynStatus writeAddr(gsport *pgsport,int talk, int listen,
    double timeout,transferState_t nextState)
{
    epicsUInt8 cmdbuf[4];
    int        lenCmd = 0;
    int        primary,secondary;

    if(talk<100) {
        cmdbuf[lenCmd++] = talk + TADBASE;
    } else {
        primary = talk / 100; secondary = talk % 100;
        cmdbuf[lenCmd++] = primary + TADBASE;
        cmdbuf[lenCmd++] = secondary + SADBASE;
    }
    if(listen<100) {
        cmdbuf[lenCmd++] = listen + LADBASE;
    } else {
        primary = listen / 100; secondary = listen % 100;
        cmdbuf[lenCmd++] = primary + LADBASE;
        cmdbuf[lenCmd++] = secondary + SADBASE;
    }
    return writeCmd(pgsport,cmdbuf,lenCmd,timeout,nextState);
}

static asynStatus writeGpib(gsport *pgsport,const char *buf, int cnt,
    int *actual, int addr, double timeout)
{
    epicsUInt8 cmdbuf[2] = {IBUNT,IBUNL};
    asynStatus status;

    *actual=0;
    pgsport->bytesRemainingWrite = cnt;
    pgsport->nextByteWrite = (epicsUInt8 *)buf;
    pgsport->status = asynSuccess;
    status = writeAddr(pgsport,0,addr,timeout,transferStateWrite);
    if(status!=asynSuccess) return status;
    *actual = cnt - pgsport->bytesRemainingWrite;
    status = pgsport->status;
    if(status!=asynSuccess) return status;
    writeCmd(pgsport,cmdbuf,2,timeout,transferStateIdle);
    return status;
}

static asynStatus readGpib(gsport *pgsport,char *buf, int cnt, int *actual,
    int addr, double timeout)
{
    epicsUInt8 cmdbuf[2] = {IBUNT,IBUNL};
    asynStatus status;

    *actual=0; *buf=0;
    pgsport->bytesRemainingRead = cnt;
    pgsport->nextByteRead = buf;
    writeRegister(pgsport,AUXMR,RHDF);
    pgsport->status = asynSuccess;
    status = writeAddr(pgsport,addr,0,timeout,transferStateRead);
    if(status!=asynSuccess) return status;
    *actual = cnt - pgsport->bytesRemainingRead;
    writeCmd(pgsport,cmdbuf,2,timeout,transferStateIdle);
    return status;
}

static void gpibPortReport(void *pdrvPvt,FILE *fd,int details)
{
    gsport *pgsport = (gsport *)pdrvPvt;

    fprintf(fd,"    gsip488 port %s vector %d carrier %d module %d "
               "base %x registers %p\n",
               pgsport->portName,pgsport->vector,pgsport->carrier,
               pgsport->module,pgsport->base,pgsport->registers);
}

static asynStatus gpibPortConnect(void *pdrvPvt,asynUser *pasynUser)
{
    gsport *pgsport = (gsport *)pdrvPvt;
    int addr = 0;
    asynStatus status;

    status = pasynManager->getAddr(pasynUser,&addr);
    if(status!=asynSuccess) return status;
    asynPrint(pasynUser, ASYN_TRACE_FLOW,
        "%s addr %d gpibPortConnect\n",pgsport->portName,addr);
    if(addr>=0) {
        pasynManager->exceptionConnect(pasynUser);
        return asynSuccess;
    }
    /* Issue a software reset to the GPIB controller chip*/
    writeRegister(pgsport,AUXMR,SWRSTS);
    epicsThreadSleep(.01);
    writeRegister(pgsport,ADR,0); /*controller address is 0*/
    writeRegister(pgsport,IMR0,0);
    writeRegister(pgsport,IMR1,0);
    readRegister(pgsport,ISR0);
    readRegister(pgsport,ISR1);
    epicsThreadSleep(.01);
    writeRegister(pgsport,AUXMR,SWRSTC);
    writeRegister(pgsport,AUXMR,IFCS);
    epicsThreadSleep(.01);
    writeRegister(pgsport,AUXMR,IFCC);
    writeRegister(pgsport,AUXMR,REMS);
    writeRegister(pgsport,AUXMR,HDFAS);
    writeRegister(pgsport,IMR0,BI|BO);
    writeRegister(pgsport,IMR1,ERR|MA|(pgsport->srqEnabled ? SRQ : 0));
    pasynManager->exceptionConnect(pasynUser);
    return asynSuccess;
}

static asynStatus gpibPortDisconnect(void *pdrvPvt,asynUser *pasynUser)
{
    gsport *pgsport = (gsport *)pdrvPvt;
    int addr = 0;
    asynStatus status;

    status = pasynManager->getAddr(pasynUser,&addr);
    if(status!=asynSuccess) return status;
    asynPrint(pasynUser, ASYN_TRACE_FLOW,
        "%s addr %d gpibPortDisconnect\n",pgsport->portName,addr);
    if(addr>=0) {
        pasynManager->exceptionDisconnect(pasynUser);
        return asynSuccess;
    }
    /* Issue a software reset to the GPIB controller chip*/
    writeRegister(pgsport,IMR0,0);
    writeRegister(pgsport,IMR1,0);
    pasynManager->exceptionDisconnect(pasynUser);
    return asynSuccess;
}

static asynStatus gpibPortSetPortOptions(void *pdrvPvt,asynUser *pasynUser,
    const char *key, const char *val)
{
    gsport *pgsport = (gsport *)pdrvPvt;

    asynPrint(pasynUser,ASYN_TRACE_FLOW,"%s gpibPortSetPortOptions\n",
        pgsport->portName);
    epicsSnprintf(pasynUser->errorMessage,pasynUser->errorMessageSize,
        "%s gpibPort does not have any options\n",pgsport->portName);
    return asynError;
}

static asynStatus gpibPortGetPortOptions(void *pdrvPvt,asynUser *pasynUser,
    const char *key, char *val,int sizeval)
{
    gsport *pgsport = (gsport *)pdrvPvt;

    asynPrint(pasynUser,ASYN_TRACE_FLOW,"%s gpibPortGetPortOptions\n",
        pgsport->portName);
    epicsSnprintf(pasynUser->errorMessage,pasynUser->errorMessageSize,
        "%s gpibPort does not have any options\n",pgsport->portName);
    return asynError;
}

static asynStatus gpibPortRead(void *pdrvPvt,asynUser *pasynUser,
    char *data,int maxchars,int *nbytesTransfered)
{
    gsport *pgsport = (gsport *)pdrvPvt;
    int        actual = 0;
    double     timeout = pasynUser->timeout;
    int        addr = 0;
    asynStatus status;

    status = pasynManager->getAddr(pasynUser,&addr);
    if(status!=asynSuccess) return status;
    asynPrint(pasynUser,ASYN_TRACE_FLOW,"%s addr %d gpibPortRead\n",
        pgsport->portName,addr);
    pgsport->errorMessage[0] = 0;
    status = readGpib(pgsport,data,maxchars,&actual,addr,timeout);
    if(status!=asynSuccess) {
        epicsSnprintf(pasynUser->errorMessage,pasynUser->errorMessageSize,
            "%s readGpib failed %s\n",pgsport->portName,pgsport->errorMessage);
    }
    asynPrintIO(pasynUser,ASYN_TRACEIO_DRIVER,
        data,actual,"%s addr %d gpibPortRead\n",pgsport->portName,addr);
    *nbytesTransfered = actual;
    return status;
}

static asynStatus gpibPortWrite(void *pdrvPvt,asynUser *pasynUser,
    const char *data,int numchars,int *nbytesTransfered)
{
    gsport *pgsport = (gsport *)pdrvPvt;
    int        actual = 0;
    double     timeout = pasynUser->timeout;
    int        addr = 0;
    asynStatus status;

    status = pasynManager->getAddr(pasynUser,&addr);
    if(status!=asynSuccess) return status;
    asynPrint(pasynUser,ASYN_TRACE_FLOW,"%s addr %d gpibPortWrite nchar %d\n",
        pgsport->portName,addr,numchars);
    pgsport->errorMessage[0] = 0;
    status = writeGpib(pgsport,data,numchars,&actual,addr,timeout);
    if(status!=asynSuccess) {
        epicsSnprintf(pasynUser->errorMessage,pasynUser->errorMessageSize,
            "%s writeGpib failed %s\n",pgsport->portName,pgsport->errorMessage);
    } else if(actual!=numchars) {
        epicsSnprintf(pasynUser->errorMessage,pasynUser->errorMessageSize,
            "%s requested %d but sent %d bytes\n",pgsport->portName,numchars,actual);
        status = asynError;
    }
    asynPrintIO(pasynUser,ASYN_TRACEIO_DRIVER,
        data,actual,"%s addr %d gpibPortWrite\n",pgsport->portName,addr);
    *nbytesTransfered = actual;
    return status;
}

static asynStatus gpibPortFlush(void *pdrvPvt,asynUser *pasynUser)
{
    /*Nothing to do */
    return asynSuccess;
}

static asynStatus gpibPortSetEos(void *pdrvPvt,asynUser *pasynUser,
    const char *eos,int eoslen)
{
    gsport     *pgsport = (gsport *)pdrvPvt;
    int        addr = 0;
    asynStatus status;

    status = pasynManager->getAddr(pasynUser,&addr);
    if(status!=asynSuccess) return status;
    asynPrint(pasynUser, ASYN_TRACE_FLOW,
        "%s addr %d gpibPortSetEos eoslen %d\n",pgsport->portName,addr,eoslen);
    if(eoslen>1 || eoslen<0) {
        asynPrint(pasynUser,ASYN_TRACE_ERROR,
           "%s addr %d gpibTi9914SetEos illegal eoslen %d\n",
           pgsport->portName,addr,eoslen);
        return asynError;
    }
    pgsport->eos = (eoslen==0) ? -1 : (int)(unsigned int)eos[0] ;
    return asynSuccess;
}
static asynStatus gpibPortGetEos(void *pdrvPvt,asynUser *pasynUser,
    char *eos, int eossize, int *eoslen)
{
    gsport *pgsport = (gsport *)pdrvPvt;

    if(eossize<1) {
        asynPrint(pasynUser,ASYN_TRACE_ERROR,
            "%s addr %d gpibPortGetEos eossize %d too small\n",
            pgsport->portName,eossize);
        *eoslen = 0;
        return asynError;
    }
    if(pgsport->eos==-1) {
        *eoslen = 0;
    } else {
        eos[0] = (unsigned int)pgsport->eos;
        *eoslen = 1;
    }
    asynPrintIO(pasynUser, ASYN_TRACE_FLOW, eos, *eoslen,
            "%s gpibPortGetEos eoslen %d\n",pgsport->portName,eoslen);
    return asynSuccess;
}

static asynStatus gpibPortAddressedCmd(void *pdrvPvt,asynUser *pasynUser,
    const char *data, int length)
{
    gsport *pgsport = (gsport *)pdrvPvt;
    double     timeout = pasynUser->timeout;
    int        addr = 0;
    int        actual;
    asynStatus status;
    epicsUInt8 cmdbuf[2] = {IBUNT,IBUNL};

    status = pasynManager->getAddr(pasynUser,&addr);
    if(status!=asynSuccess) return status;
    asynPrint(pasynUser,ASYN_TRACE_FLOW,"%s addr %d gpibPortAddressedCmd nchar %d\n",
        pgsport->portName,addr,length);
    pgsport->errorMessage[0] = 0;
    status = writeAddr(pgsport,0,addr,timeout,transferStateIdle);
    if(status==asynSuccess) {
        status=writeCmd(pgsport,data,length,timeout,transferStateIdle);
    }
    if(status!=asynSuccess) {
        epicsSnprintf(pasynUser->errorMessage,pasynUser->errorMessageSize,
            "%s writeGpib failed %s\n",pgsport->portName,pgsport->errorMessage);
    }
    actual = length - pgsport->bytesRemainingCmd;
    asynPrintIO(pasynUser,ASYN_TRACEIO_DRIVER,
        data,actual,"%s gpibPortAddressedCmd\n",pgsport->portName);
    if(status!=asynSuccess) return status;
    writeCmd(pgsport,cmdbuf,2,timeout,transferStateIdle);
    return status;
}

static asynStatus gpibPortUniversalCmd(void *pdrvPvt, asynUser *pasynUser, int cmd)
{
    gsport *pgsport = (gsport *)pdrvPvt;
    double     timeout = pasynUser->timeout;
    asynStatus status;
    char   buffer[1];

    asynPrint(pasynUser,ASYN_TRACE_FLOW,"%s gpibPortUniversalCmd %2.2x\n",
        pgsport->portName,cmd);
    pgsport->errorMessage[0] = 0;
    buffer[0] = cmd;
    status = writeCmd(pgsport,buffer,1,timeout,transferStateIdle);
    if(status!=asynSuccess) {
        epicsSnprintf(pasynUser->errorMessage,pasynUser->errorMessageSize,
            "%s writeGpib failed %s\n",pgsport->portName,pgsport->errorMessage);
    }
    asynPrintIO(pasynUser,ASYN_TRACEIO_DRIVER,
        buffer,1,"%s gpibPortUniversalCmd\n",pgsport->portName);
    return status;
}

static asynStatus gpibPortIfc(void *pdrvPvt, asynUser *pasynUser)
{
    gsport *pgsport = (gsport *)pdrvPvt;

    asynPrint(pasynUser, ASYN_TRACE_FLOW,
        "%s gpibPortIfc\n",pgsport->portName);
    writeRegister(pgsport,AUXMR,IFCS);
    epicsThreadSleep(.01);
    writeRegister(pgsport,AUXMR,IFCC);
    return asynSuccess;
}

static asynStatus gpibPortRen(void *pdrvPvt,asynUser *pasynUser, int onOff)
{
    gsport *pgsport = (gsport *)pdrvPvt;

    asynPrint(pasynUser, ASYN_TRACE_FLOW,
        "%s gpibPortRen %s\n",pgsport->portName,(onOff ? "On" : "Off"));
    writeRegister(pgsport,AUXMR,(onOff ? REMS : REMC));
    return asynSuccess;
}

static int gpibPortSrqStatus(void *pdrvPvt)
{
    gsport *pgsport = (gsport *)pdrvPvt;
    int    srqHappened = pgsport->srqHappened;

    pgsport->srqHappened = epicsFalse;
    return srqHappened;
}

static asynStatus gpibPortSrqEnable(void *pdrvPvt, int onOff)
{
    gsport *pgsport = (gsport *)pdrvPvt;

    pgsport->srqEnabled = (onOff ? epicsTrue : epicsFalse);
    writeRegister(pgsport,IMR1,ERR|MA|(pgsport->srqEnabled ? SRQ : 0));
    return asynSuccess;
}

static asynStatus gpibPortSerialPollBegin(void *pdrvPvt)
{
    gsport     *pgsport = (gsport *)pdrvPvt;
    double     timeout = 1.0;
    asynStatus status;
    char       cmd[1];

    cmd[0] = IBSPE;
    status = writeCmd(pgsport,cmd,1,timeout,transferStateIdle);
    return status;
}

static int gpibPortSerialPoll(void *pdrvPvt, int addr, double timeout)
{
    gsport     *pgsport = (gsport *)pdrvPvt;
    epicsUInt8 buffer[1];
    int        actual = 0;

    readGpib(pgsport,buffer,1,&actual,addr,timeout);
    return (actual==1 ? buffer[0] : 0);
}

static asynStatus gpibPortSerialPollEnd(void *pdrvPvt)
{
    gsport     *pgsport = (gsport *)pdrvPvt;
    double     timeout = 1.0;
    asynStatus status;
    char       cmd[1];

    cmd[0] = IBSPD;
    status = writeCmd(pgsport,cmd,1,timeout,transferStateIdle);
    return status;
}

int gsIP488Configure(char *portName,int carrier, int module, int vector,
    unsigned int priority, int noAutoConnect)
{
    gsport *pgsport;
    int ipstatus;
    epicsUInt8 *registers;
    int size;

    ipstatus = ipmValidate(carrier,module,GREEN_SPRING_ID,GSIP_488);
    if(ipstatus) {
        printf("gsIP488Configure Unable to validate IP module");
        return 0;
    }
    registers = (char *)ipmBaseAddr(carrier, module, ipac_addrIO);
    if(!registers) {
        printf("gsIP488Configure no memory allocated "
            "for carrier %d module %d\n", carrier,module);
        return 0;
    }
    if((vector&1)==0) {
        printf("gsIP488Configure vector MUST be odd. "
            "It is changed from %d to %d\n", vector, vector|1);
        vector |= 1;
    }
    size = sizeof(gsport) + strlen(portName)+1;
    pgsport = callocMustSucceed(size,sizeof(char),"gsIP488Configure");
    pgsport->carrier = carrier;
    pgsport->module = module;
    pgsport->vector = vector;
    pgsport->portName = (char *)(pgsport+1);
    strcpy(pgsport->portName,portName);
    pgsport->waitForInterrupt = epicsEventMustCreate(epicsEventEmpty);
    callbackSetCallback(srqCallback,&pgsport->callback);
    callbackSetUser(pgsport,&pgsport->callback);
    pgsport->registers = registers;
    ipstatus = ipmIntConnect(carrier,module,vector,gsip488,(int)pgsport);
    if(ipstatus) {
        printf("gsIP488Configure ipmIntConnect failed\n");
        return asynError;
    }
    ipmIrqCmd(carrier, module, 0, ipac_irqEnable);
    writeRegister(pgsport,VECTOR,pgsport->vector);
    pgsport->asynGpibPvt = pasynGpib->registerPort(pgsport->portName,
        1,!noAutoConnect, &gpibPort,pgsport,priority,0);
    return 0;
}

/* IOC shell command registration */
#include <iocsh.h>
static const iocshArg gsIP488ConfigureArg0 = { "portName",iocshArgString};
static const iocshArg gsIP488ConfigureArg1 = { "carrier",iocshArgInt};
static const iocshArg gsIP488ConfigureArg2 = { "module",iocshArgInt};
static const iocshArg gsIP488ConfigureArg3 = { "vector",iocshArgInt};
static const iocshArg gsIP488ConfigureArg4 = { "priority",iocshArgInt};
static const iocshArg gsIP488ConfigureArg5 = { "disable auto-connect",iocshArgInt};
static const iocshArg *gsIP488ConfigureArgs[] = {&gsIP488ConfigureArg0,
    &gsIP488ConfigureArg1, &gsIP488ConfigureArg2, &gsIP488ConfigureArg3,
    &gsIP488ConfigureArg4,&gsIP488ConfigureArg5};
static const iocshFuncDef gsIP488ConfigureFuncDef = {"gsIP488Configure",6,gsIP488ConfigureArgs};
static void gsIP488ConfigureCallFunc(const iocshArgBuf *args)
{
    gsIP488Configure (args[0].sval, args[1].ival, args[2].ival, args[3].ival,
                                                args[4].ival, args[5].ival);
}

/*
 * This routine is called before multitasking has started, so there's
 * no race condition in the test/set of firstTime.
 */
static void gsIP488RegisterCommands (void)
{
    static int firstTime = 1;
    if (firstTime) {
        firstTime = 0;
        iocshRegister(&gsIP488ConfigureFuncDef,gsIP488ConfigureCallFunc);
    }
}
epicsExportRegistrar(gsIP488RegisterCommands);
