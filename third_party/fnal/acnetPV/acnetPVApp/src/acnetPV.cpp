//************************************************************************//
//*                                                                      *//
//* Written by Pierrick Hanlet, 22 October 2019                          *//
//*                                                                      *//
//* Basic EPICS structure to create basic acnet device from EPICS PVs    *//
//* Description of the struct is as follows:                             *//
//************************************************************************//

#include "acnetPV.h"

static FieldCreatePtr     fieldCreate     = getFieldCreate();
static StandardFieldPtr   standardField   = getStandardField();
static PVDataCreatePtr    pvDataCreate    = getPVDataCreate();
static StandardPVFieldPtr standardPVField = getStandardPVField();
static PvaClientPtr       pva             = PvaClient::get("pva ca");


//************************************************************************//
//* Class definitions                                                    *//
//************************************************************************//
ACNET::PV::PV(std::string const & recordName,
	      epics::pvData::PVStructurePtr const & pvStructure) :
              PVRecord(recordName,pvStructure) {

  // Read values
  ptrName           = pvStructure->getSubField<PVString>("Name.value");
  ptrDescription    = pvStructure->getSubField<PVString>("Description.value");
  ptrOwner          = pvStructure->getSubField<PVString>("Owner.value");
  ptrBasicStatus    = pvStructure->getSubField<PVShort>("BasicStatus.value");
  ptrExtendedStatus = pvStructure->getSubField<PVInt>("ExtendedStatus.value");
  ptrDigitalStatus  = pvStructure->getSubField<PVShort>("DigitalStatus.value");
  ptrAnalogStatus   = pvStructure->getSubField<PVFloat>("AnalogStatus.value");
  ptrReading        = pvStructure->getSubField<PVDoubleArray>("Reading.value");
  ptrEGU            = pvStructure->getSubField<PVString>("EGU.value");

  // Set values
  ptrControl        = pvStructure->getSubField<PVShort>("Control.value");
  ptrSetting        = pvStructure->getSubField<PVFloat>("Setting.value");

  // Timestamps
  tsBasicStatus.attach(pvStructure->
		       getSubFieldT<PVStructure>("BasicStatus.timeStamp"));
  tsExtendedStatus.attach(pvStructure->-L/home/hanlet/epicsDEV/Support/acnetPV/lib/linux-x86_64
			  getSubFieldT<PVStructure>("ExtendedStatus.timeStamp"));
  tsDigitalStatus.attach(pvStructure->
			 getSubFieldT<PVStructure>("DigitalStatus.timeStamp"));
  tsAnalogStatus.attach(pvStructure->
			getSubFieldT<PVStructure>("AnalogStatus.timeStamp"));
  tsReading.attach(pvStructure->
		   getSubFieldT<PVStructure>("Reading.timeStamp"));
  tsControl.attach(pvStructure->
		   getSubFieldT<PVStructure>("Control.timeStamp"));
  tsSetting.attach(pvStructure->
		   getSubFieldT<PVStructure>("Setting.timeStamp"));

  // Initialize change variables
  oldReading = -9999.9;
  oldControl = -1;
  oldSetting = 0.0;

}

ACNET::PV::~PV() {
}

//************************************************************************//
//* Functions                                                            *//
//************************************************************************//
short ACNET::PV::initPVs(aSubRecord *pvData) {

  static bool first=true;
  if (first) {
    first=false;
    return(0);
  }

  cout << "Initializing " << (char*)pvData->a << endl;

  return(0);
}


short ACNET::PV::readPVs(aSubRecord *pvData) {

  static bool first=true;
  if (first) {
    first=false;
    return(0);
  }

  try {
    // Get timestamp
    TimeStamp timeStamp;
    timeStamp.getCurrent();

    // Create pointers to Acnet Device Record (adr) sub-structure values
    PVRecordPtr pvr = PVDatabase::getMaster()->findRecord((char*)pvData->a);
    if (pvr) {
      PV *adrPtr = dynamic_cast<ACNET::PV*>(pvr.get());

      // Whilst busy, nobody touch this
      pvr->lock();
      pvr->beginGroupPut();
      PVStructurePtr pvStructure = pvr->getPVStructure();
      if (pvStructure) {

	//	  string type = pvData->fth;
	shared_vector<double> ReadArray;

	// Fill values in Acnet Device Record structure if values change
	static bool init=true;
	if (init) {
	  adrPtr->ptrName->put((char*)pvData->a);
	  if (pvData->nog == 1) {
	    adrPtr->ptrDescription->put((char*)pvData->h);
	    adrPtr->ptrEGU->put((char*)pvData->i);
	  }
	  else {
	    adrPtr->ptrDescription->put((char*)pvData->j);
	    adrPtr->ptrEGU->put((char*)pvData->k);
	  }
	  init=false;
	}

	if (adrPtr->ptrBasicStatus->get() != *(short*)pvData->b) {
	  adrPtr->ptrBasicStatus->put(*(short*)pvData->b);
	  adrPtr->tsBasicStatus.set(timeStamp);
	}

	if (adrPtr->ptrExtendedStatus->get() != *(int*)pvData->c) {
	  adrPtr->ptrExtendedStatus->put(*(int*)pvData->c);
	  adrPtr->tsExtendedStatus.set(timeStamp);
	}

	if (adrPtr->ptrDigitalStatus->get() != *(short*)pvData->d) {
	  adrPtr->ptrDigitalStatus->put(*(short*)pvData->d);
	  adrPtr->tsDigitalStatus.set(timeStamp);
	}

	if (adrPtr->ptrAnalogStatus->get() != *(double*)pvData->e) {
	  adrPtr->ptrAnalogStatus->put(*(double*)pvData->e);
	  adrPtr->tsAnalogStatus.set(timeStamp);
	}
	// Reading is an array
	if (pvData->nog > 1) {
	  for (epicsUInt32 ndx=0; ndx<pvData->nog; ndx++) {
	    ReadArray.push_back(((double*)pvData->g)[ndx]);
	  }
	  adrPtr->ptrReading->replace(freeze(ReadArray));
	  adrPtr->tsReading.set(timeStamp);
	}
	// Reading is a scalar
	else {
	  if (adrPtr->oldReading != *(double*)pvData->f) {
	    adrPtr->oldReading = *(double*)pvData->f;
	    ReadArray.push_back(*(double*)pvData->f);
	    adrPtr->ptrReading->replace(freeze(ReadArray));
	    adrPtr->tsReading.set(timeStamp);
	  }
	}

	// Complete writing, release record
	pvr->endGroupPut();
	pvr->unlock();
      }

      // Failure to find pvStructure
      else {
	cout << "readPVs: Failed to find pvStructure" << endl;
      }
    }

    // Failure to find pvr
    else {
      cout << "readPVs: Failed to find record pointer for "
	   <<(char*)pvData->a << endl;
    }
  }

  // Failed read
  catch(...) {
    throw epics::pvAccess::RPCRequestException(Status::STATUSTYPE_ERROR,"readPVs error");
  }

  return(0);
}



short ACNET::PV::setPVs(aSubRecord *pvData) {

  static bool first=true;
  if (first) {
    first=false;
    return(0);
  }

  try {
    // Get timestamp
    TimeStamp timeStamp;
    timeStamp.getCurrent();

    // Create pointers to Acnet Device Record sub-structure values
    PVRecordPtr pvr = PVDatabase::getMaster()->findRecord((char*)pvData->a);
    if (pvr) {
      PV *adrPtr = dynamic_cast<ACNET::PV*>(pvr.get());

      // Whilst busy, nobody touch this
      pvr->lock();
      PVStructurePtr pvStructure = pvr->getPVStructure();
      if (pvStructure) {
	if (adrPtr->ptrControl->get() != adrPtr->oldControl) {
	  *(short*)pvData->vala = adrPtr->ptrControl->get();
	  adrPtr->oldControl =  adrPtr->ptrControl->get();
	  adrPtr->tsControl.set(timeStamp);
	}
	if (adrPtr->ptrSetting->get() != adrPtr->oldSetting) {
	  *(float*)pvData->valb = adrPtr->ptrSetting->get();
	  adrPtr->oldSetting = adrPtr->ptrSetting->get();
	  adrPtr->tsSetting.set(timeStamp);
	}
      }

      // Complete writing, release record
      pvr->unlock();
    }

    // Failure to find pvr
    else {
      cout << "setPVs: Failed to find record pointer" << endl;
    }
  }

  // Failed read
  catch(...) {
    throw epics::pvAccess::RPCRequestException(Status::STATUSTYPE_ERROR,"setPVs error");
  }

  return(0);
}
/*
void ACNET::PV::StartMonitor(PvaClientPtr const &pva,
			     string const &recordName,
			     string const &provider) {
  cout << "recordName=" << recordName << " and provider=" << provider << endl;
  cout << "StartMonitor: 1\n";
  PvaClientMonitorPtr monitor = pva->channel(recordName,provider,2.0)->monitor("");
  cout << "StartMonitor: 2\n";
  PvaClientMonitorDataPtr monitorData = monitor->getData();

  cout << "StartMonitor: 3\n";
  PvaClientPutPtr put = pva->channel(recordName,provider,2.0)->put("");
  cout << "StartMonitor: 4\n";
  PvaClientPutDataPtr putData = put->getData();

  cout << "StartMonitor: 5\n";
  for(size_t ntimes=0; ntimes<5; ++ntimes) {
    double value = ntimes;
    cout << "put " << value << endl;
    putData->putDouble(value); put->put();
    if(!monitor->waitEvent(.1)) {
      cout << "waitEvent returned false. Why???";
      continue;
    } else while(true) {
	cout << "monitor " << monitorData->getDouble() << endl;
	cout << "changed\n";
	monitorData->showChanged(cout);
	cout << "overrun\n";
	monitorData->showOverrun(cout);
	monitor->releaseEvent();
	if(!monitor->poll()) break;
      }
  }
}
*/



////////////////////////////////////////////////////////////////////////////
// Create record creation function from st.cmd and register
static const iocshArg testArg0 = { "recordName", iocshArgString };
static const iocshArg *testArgs[] = {&testArg0};
static const iocshFuncDef AcnetDeviceRecordFuncDef = {"createAcnetDeviceRecord", 1,testArgs};

static void AcnetDeviceRecordCallFunc(const iocshArgBuf *args) {
  char *recordName = args[0].sval;
  if(!recordName) { 
    throw std::runtime_error("createAcnetDeviceRecord: invalid number of arguments");
  }
  ACNET::PV::PVPtr record = ACNET::PV::create(recordName);
  bool result = PVDatabase::getMaster()->addRecord(record);
  if (!result) {
    cout << "recordname " << recordName << " not added" << endl;
  }
  else {
    usleep(50000);
  }
}
static void AcnetDeviceRecordRegister(void) {
  static int firstTime = 1;
  if (firstTime) {
    firstTime = 0;
    iocshRegister(&AcnetDeviceRecordFuncDef, AcnetDeviceRecordCallFunc);
  }
}


////////////////////////////////////////////////////////////////////////////
// Define structure which carries acnet device record properties
ACNET::PV::PVPtr ACNET::PV::create(string const &recordName) {
    PVStructurePtr pvStructure = createStruct();
    PVPtr pvRecord(new PV(recordName,pvStructure));
    pvRecord->initPVRecord();
    return pvRecord;
}
PVStructurePtr ACNET::PV::createStruct() {
  return PVStructurePtr(
    new PVStructure(fieldCreate->createFieldBuilder()->
    setId("fermi:nt/AcnetDevice:1.0")->
    addNestedStructure("DeviceIndex") ->
      add("value",pvInt) ->
    endNested()->
    addNestedStructure("SSDN") ->
      add("value",pvUByte) ->
    endNested()->
    addNestedStructure("Node") ->
      add("value",pvString) ->
    endNested()->
    addNestedStructure("Name") ->
      add("value",pvString) ->
    endNested()->
    addNestedStructure("Description") ->
      add("value",pvString) ->
    endNested()->
    addNestedStructure("Owner") ->
      add("value",pvString) ->
    endNested()->
    addNestedStructure("BasicStatus") ->
      add("timeStamp",standardField->timeStamp()) ->
      add("value",pvShort) ->
    endNested()->
    addNestedStructure("ExtendedStatus") ->
      add("timeStamp",standardField->timeStamp()) ->
      add("value",pvInt) ->
    endNested()->
    addNestedStructure("Control") ->
      add("timeStamp",standardField->timeStamp()) ->
      add("value",pvShort) ->
    endNested()->
    addNestedStructure("DigitalStatus") ->
      add("timeStamp",standardField->timeStamp()) ->
      add("value",pvShort) ->
      add("alarm",standardField->alarm()) ->
    endNested()->
    addNestedStructure("AnalogStatus") ->
      add("timeStamp",standardField->timeStamp()) ->
      add("value",pvFloat) ->
      add("alarm",standardField->alarm()) ->
    endNested()->
    addNestedStructure("Reading") ->
      add("timeStamp",standardField->timeStamp()) ->
      addArray("value",pvDouble) ->
      add("alarm",standardField->alarm()) ->
    endNested()->
    addNestedStructure("Setting") ->
      add("timeStamp",standardField->timeStamp()) ->
      add("value",pvFloat) ->
    endNested()->
    addNestedStructure("EGU") ->
      add("value",pvString) ->
    endNested()->

    // Create Structure
    createStructure()));
}

////////////////////////////////////////////////////////////////////////////
// Functions which use these records


//************************************************************************//
//* Register Functions                                                   *//
//************************************************************************//
extern "C" {
  short CallInitPVs(aSubRecord *pvData) {
    return ACNET::PV::initPVs(pvData);
  }
  short CallReadPVs(aSubRecord *pvData) {
    return ACNET::PV::readPVs(pvData);
  }
  short CallSetPVs(aSubRecord *pvData) {
    return ACNET::PV::setPVs(pvData);
  }
  epicsExportRegistrar(AcnetDeviceRecordRegister);
  epicsExportRegistrar(CallInitPVs);
  epicsExportRegistrar(CallReadPVs);
  epicsExportRegistrar(CallSetPVs);
}
