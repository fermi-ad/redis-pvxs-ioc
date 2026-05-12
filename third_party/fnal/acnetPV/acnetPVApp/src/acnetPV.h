//************************************************************************//
//*                                                                      *//
//* Written by Pierrick Hanlet, 22 October 2019                          *//
//*                                                                      *//
//* Basic EPICS structure to create basic acnet device from EPICS PVs    *//
//* Description of the struct is as follows:                             *//
//************************************************************************//
//*                                                                      *//
//* - DeviceIndex is acnet DI                                            *//
//* - SSDN is acnet SSDN                                                 *//
//* - Node is acnet trunk/node                                           *//
//* - Name is acnet device name                                          *//
//* - Description is a description of the field device                   *//
//* - BasicStatus word is:                                               *//
//*     bit0 - Off/On (0/1)                                              *//
//*     bit1 - Disabled/Enabled (0/1)                                    *//
//*     bit2 - Local/Remote (0/1)                                        *//
//*     bit3 - Polarity +/- (0/1)                                        *//
//* - ExtendedStatus is user defined word                                *//
//* - Control word is:                                                   *//
//*     bit0 - Off/On (0/1)                                              *//
//*     bit1 - Disabled/Enabled (0/1)                                    *//
//*     bit2 - Local/Remote (0/1)                                        *//
//*     bit3 - Polarity +/- (0/1)                                        *//
//* - DigitalStatus is user defined word carrying interlocks             *//
//* - AnalogStatus is array of floats [SSDN a/b/c,alarm HIHI,alarm LOLO] *//
//* - Reading is array of floats, each packing 4-bytes                   *//
//* - Setting is array of floats, each packing 4-bytes                   *//
//* - EGU is engineering units                                           *//
//*                                                                      *//
//************************************************************************//
#ifndef acnetPV_H
#define acnetPV_H

#include <iostream>
#include <unordered_map>
#include <memory>

#include <stddef.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#include <epicsTime.h>
#include <epicsExport.h>
#include <registryFunction.h>

#include <iocsh.h>
#include <pv/pvData.h>
#include <pv/pvDatabase.h>
#include <pv/standardField.h>
#include <pv/standardPVField.h>
#include <pv/pvdbcrAddRecord.h>
#include <pv/pvTimeStamp.h>
#include <pv/pvaClient.h>

#include <aSubRecord.h>

using namespace std;
using namespace epics::pvData;
using namespace epics::pvDatabase;
using namespace epics::pvaClient;

namespace ACNET {

  class PV : public PVRecord {
    
  public:
    // Pointer definitions
    POINTER_DEFINITIONS(PV);
    typedef shared_pointer PVPtr;
    typedef std::tr1::shared_ptr<PVArray> PVArrayPtr;
    typedef std::tr1::shared_ptr<ACNET::PV> MonitorLinkRecordPtr;
    static PVPtr create(string const &recordName);

    // Constructors & Destructor
    PV(std::string const & recordName,
       epics::pvData::PVStructurePtr const & pvStructure);
    ~PV();

    // Functions
    static int   initAcnetDeviceRecord(aSubRecord*);
    static short initPVs(aSubRecord*);  // initialization
    static short readPVs(aSubRecord*);  // reads V3 PVs
    static short setPVs(aSubRecord*);   // set V3 PVs

  private:
    // Change variables
    double oldReading;
    short  oldControl;
    float  oldSetting;

    // Read Substructures
    PVStringPtr      ptrName;
    PVStringPtr      ptrDescription;
    PVStringPtr      ptrOwner;
    PVShortPtr       ptrBasicStatus;
    PVIntPtr         ptrExtendedStatus;
    PVShortPtr       ptrDigitalStatus;
    PVFloatPtr       ptrAnalogStatus;
    PVDoubleArrayPtr ptrReading;
    PVStringPtr      ptrEGU;


    // Set Substructures
    PVShortPtr  ptrControl;
    PVFloatPtr  ptrSetting;

    // Time Stamps
    PVTimeStamp tsBasicStatus;
    PVTimeStamp tsExtendedStatus;
    PVTimeStamp tsDigitalStatus;
    PVTimeStamp tsAnalogStatus;
    PVTimeStamp tsReading;
    PVTimeStamp tsControl;
    PVTimeStamp tsSetting;

    // Monitors
    //    MonitorLinkRecordPtr MonitorLinkRecord;
    //    bool isConnected;

    // Methods
    static PVStructurePtr createStruct();
    /*
    static void StartMonitor(PvaClientPtr const &pva,
			     string const & recordName,
			     string const& provider);
    void monitorConnect(const Status& status,
    			PvaClientMonitorPtr const & monitor,
    			StructureConstPtr const & structure);
    */
    
  };
}

#endif

// Local variables:
// mode: c++
// end:
