//************************************************************************//
//*                                                                      *//
//* Written by Pierrick Hanlet, 22 October 2019                          *//
//* Written by Pierrick Hanlet, 8 August 2025 using PVXS api             *//
//*                                                                      *//
//* Basic EPICS structure to create basic acnet device from EPICS PVs    *//
//************************************************************************//
#include "acnetPVXS.h"

using namespace pvxs;
using namespace pvxs::server;
using namespace ACNET;
using std::string;
using std::cout;
using std::cerr;
using std::endl;
using std::shared_ptr;

//************************************************************************//
//* Helper functions                                                     *//
//************************************************************************//
namespace {

  // Safe C-string to std::string conversion (handles nullptr)
  inline std::string sstr(const char* p)
  {
    return p ? std::string(p) : std::string();
  }

  // Write timestamp struct fields matching your schema
  inline void writeTS(Value& root, const std::string& tsFieldPrefix,
		      epicsUInt32 sec, epicsUInt32 nsec, epicsInt32 userTag = 0)
  {
    root[tsFieldPrefix + ".secondsPastEpoch"] = static_cast<int64_t>(sec);
    root[tsFieldPrefix + ".nanoseconds"]      = static_cast<int32_t>(nsec);
    root[tsFieldPrefix + ".userTag"]          = static_cast<int32_t>(userTag);
  }
  
  // Defensive check for array length to prevent ridiculous allocations
  // cap at ~1M elements
  inline size_t clampCount(epicsUInt32 n, size_t max = 1u<<20)
  {
    return n > max ? max : static_cast<size_t>(n);
  }

} // namespace

//************************************************************************//
//* Define ACNET structure                                               *//
//************************************************************************//
namespace {
  const TypeDef timeStampDef (
    pvxs::TypeCode::Struct, "",
    {
      pvxs::Member(pvxs::TypeCode::Int64, "secondsPastEpoch"),
      pvxs::Member(pvxs::TypeCode::Int32, "nanoseconds"),
      pvxs::Member(pvxs::TypeCode::Int32, "userTag")
    }
  );
}

// Define the ACNET structure type
const TypeDef ACNET::PV::acnetTypeDef (
    pvxs::TypeCode::Struct,
    "fermi:nt/AcnetDevice:1.0",
    {
      pvxs::Member(pvxs::TypeCode::Int32,  "DeviceIndex"),
      pvxs::Member(pvxs::TypeCode::UInt8,  "SSDN"),
      pvxs::Member(pvxs::TypeCode::String, "Node"),
      pvxs::Member(pvxs::TypeCode::String, "Name"),
      pvxs::Member(pvxs::TypeCode::String, "Description"),
      pvxs::Member(pvxs::TypeCode::String, "Owner"),
      pvxs::Member(pvxs::TypeCode::String, "EGU"),
      pvxs::Member(pvxs::TypeCode::String, "Reader"),

      pvxs::Member(pvxs::TypeCode::Struct, "BasicStatus", {
	  pvxs::Member(pvxs::TypeCode::Int16, "value"),
	  pvxs::Member(pvxs::TypeCode::Struct, "timeStamp", {
	      pvxs::Member(pvxs::TypeCode::Int64, "secondsPastEpoch"),
	      pvxs::Member(pvxs::TypeCode::Int32, "nanoseconds"),
	      pvxs::Member(pvxs::TypeCode::Int32, "userTag")
	    })
	}),
      pvxs::Member(pvxs::TypeCode::Struct, "ExtendedStatus", {
	  pvxs::Member(pvxs::TypeCode::Int32, "value"),
	  pvxs::Member(pvxs::TypeCode::Struct, "timeStamp", {
	      pvxs::Member(pvxs::TypeCode::Int64, "secondsPastEpoch"),
	      pvxs::Member(pvxs::TypeCode::Int32, "nanoseconds"),
	      pvxs::Member(pvxs::TypeCode::Int32, "userTag")
	    })
	}),
      pvxs::Member(pvxs::TypeCode::Struct, "DigitalStatus", {
	  pvxs::Member(pvxs::TypeCode::Int16, "value"),
	  pvxs::Member(pvxs::TypeCode::Struct, "timeStamp", {
	      pvxs::Member(pvxs::TypeCode::Int64, "secondsPastEpoch"),
	      pvxs::Member(pvxs::TypeCode::Int32, "nanoseconds"),
	      pvxs::Member(pvxs::TypeCode::Int32, "userTag")
	    })
	}),
      pvxs::Member(pvxs::TypeCode::Struct, "AnalogStatus", {
	  pvxs::Member(pvxs::TypeCode::Float32, "value"),
	  pvxs::Member(pvxs::TypeCode::Struct, "timeStamp", {
	      pvxs::Member(pvxs::TypeCode::Int64, "secondsPastEpoch"),
	      pvxs::Member(pvxs::TypeCode::Int32, "nanoseconds"),
	      pvxs::Member(pvxs::TypeCode::Int32, "userTag")
	    })
	}),
      pvxs::Member(pvxs::TypeCode::Struct, "Reading", {
	  pvxs::Member(pvxs::TypeCode::Float64A, "value"),
	  pvxs::Member(pvxs::TypeCode::Float64,  "scalar"),
	  pvxs::Member(pvxs::TypeCode::Struct, "timeStamp", {
	      pvxs::Member(pvxs::TypeCode::Int64, "secondsPastEpoch"),
	      pvxs::Member(pvxs::TypeCode::Int32, "nanoseconds"),
	      pvxs::Member(pvxs::TypeCode::Int32, "userTag")
	    })
	}),
      pvxs::Member(pvxs::TypeCode::Struct, "Setting", {
	  pvxs::Member(pvxs::TypeCode::Float32, "value"),
	  pvxs::Member(pvxs::TypeCode::Struct, "timeStamp", {
	      pvxs::Member(pvxs::TypeCode::Int64, "secondsPastEpoch"),
	      pvxs::Member(pvxs::TypeCode::Int32, "nanoseconds"),
	      pvxs::Member(pvxs::TypeCode::Int32, "userTag")
	    })
	}),
      pvxs::Member(pvxs::TypeCode::Struct, "Control", {
	  pvxs::Member(pvxs::TypeCode::Int16, "value"),
	  pvxs::Member(pvxs::TypeCode::Struct, "timeStamp", {
	      pvxs::Member(pvxs::TypeCode::Int64, "secondsPastEpoch"),
	      pvxs::Member(pvxs::TypeCode::Int32, "nanoseconds"),
	      pvxs::Member(pvxs::TypeCode::Int32, "userTag")
	    })
	})

    }
);

//************************************************************************//
//* Constructor & Destructor                                             *//
//************************************************************************//
PV::PV(const string& recordName)
  : name(recordName),
    sharedpv(SharedPV::buildMailbox()),
    value(acnetTypeDef.create())
{
  // Zero timestamp shortcut
  auto zeroTS = [&](const char* base) {
    value[std::string(base)+".secondsPastEpoch"] = int64_t(0);
    value[std::string(base)+".nanoseconds"]      = int32_t(0);
    value[std::string(base)+".userTag"]          = int32_t(0);
  };
    
  // Initialize acnetPV
  value["Name"]        = name;
  value["Description"] = std::string();
  value["Owner"]       = std::string();
  value["EGU"]         = std::string();

  value["DeviceIndex"] = int32_t(0);
  value["SSDN"]        = uint8_t(0);
  value["Node"]        = std::string();

  value["BasicStatus.value"]    = int16_t(0);
  value["ExtendedStatus.value"] = int32_t(0);
  value["DigitalStatus.value"]  = int16_t(0);
  value["AnalogStatus.value"]   = float(0.0f);
  value["Reading.value"]        = pvxs::shared_array<const double>().freeze();
  value["Reading.scalar"]       = 0.0;
  value["Setting.value"]        = this->oldSetting;
  value["Control.value"]        = this->oldControl;

  zeroTS("BasicStatus.timeStamp");
  zeroTS("ExtendedStatus.timeStamp");
  zeroTS("DigitalStatus.timeStamp");
  zeroTS("AnalogStatus.timeStamp");
  zeroTS("Reading.timeStamp");
  zeroTS("Setting.timeStamp");
  zeroTS("Control.timeStamp");

  // Accept puts to Setting.value and Control.value
  sharedpv.onPut([this](SharedPV& pv, std::unique_ptr<pvxs::server::ExecOp>&& op, pvxs::Value&& put)
  {
    try {
      Value cur = pv.fetch();
      bool flag = false;

      auto nowTS = [&](const char* base) {
	epicsTimeStamp now{}; epicsTimeGetCurrent(&now);
	cur[std::string(base)+".secondsPastEpoch"] = int64_t(now.secPastEpoch);
	cur[std::string(base)+".nanoseconds"]      = int32_t(now.nsec);
	cur[std::string(base)+".userTag"]          = int32_t(0);
      };

      // Basic/Extended/Digital/Analog status
      if (put["BasicStatus.value"].isMarked(true, true)) {
	cur["BasicStatus.value"] = put["BasicStatus.value"].as<int16_t>();
	nowTS("BasicStatus.timeStamp");
	flag = true;
      }
      if (put["ExtendedStatus.value"].isMarked(true, true)) {
	cur["ExtendedStatus.value"] = put["ExtendedStatus.value"].as<int32_t>();
	nowTS("ExtendedStatus.timeStamp");
	flag = true;
      }
      if (put["DigitalStatus.value"].isMarked(true, true)) {
	cur["DigitalStatus.value"] = put["DigitalStatus.value"].as<int16_t>();
	nowTS("DigitalStatus.timeStamp");
	flag = true;
      }
      if (put["AnalogStatus.value"].isMarked(true, true)) {
	cur["AnalogStatus.value"] = put["AnalogStatus.value"].as<float>();
	nowTS("AnalogStatus.timeStamp");
	flag = true;
      }

      // Reading.value (Float64A) — accept scalar or array
      if (put["Reading.value"].isMarked(true, true)) {
	pvxs::Value src = put["Reading.value"];
	bool handled = false;
	
	// Waveform value
	if (src.type()==pvxs::TypeCode::Float64A) {
	  auto input = src.as<pvxs::shared_array<const double>>();
	  pvxs::shared_array<double> tmparr(input.begin(),input.end());
	  cur["Reading.value"]  = tmparr.freeze();
	  cur["Reading.scalar"] = 0.0;
	  handled = true;
	}

	// Scalar value
	else if (src.type()==pvxs::TypeCode::Float64) {
	  double tmpval = src.as<double>();
	  pvxs::shared_array<double> tmparr({tmpval});
	  cur["Reading.value"]  = tmparr.freeze();
	  cur["Reading.scalar"] = tmpval;
	  handled = true;
	}

	// Input is string-like
	else if (src.type()==pvxs::TypeCode::String) {
	  std::string s = src.as<std::string>();
	  std::string buf;
	  buf.reserve(s.size());
	  for (char c : s) {
	    if (c=='[' || c==']' || c=='{' || c=='}' || c=='(' || c==')') continue;
	    if (c==',' || c=='\t' || c=='\n' || c=='\r') {
	      buf.push_back(' ');
	    }
	    else {
	      buf.push_back(c);
	    }

	    // Parse doubles
	    std::vector<double> vals;
	    std::istringstream is(buf);
	    for (double v; is >> v; ) vals.push_back(v);
	    
	    if (!vals.empty()) {
	      pvxs::shared_array<double> arr(vals.size());
	      for (size_t i=0; i<vals.size(); ++i) {
		arr[i] = vals[i];
	      }
	      cur["Reading.value"]  = arr.freeze();
	      cur["Reading.scalar"] = 0.0;
	      handled = true;
	    }
	  }

	  // Still not handled
	  if (!handled) {
	    op->error("Reading.value: unsupported input type");
	    return;
	  }
	}
	nowTS("Reading.timeStamp");
	flag = true;
      }

      // Reading.scalar (Float64) — accept scalar or array
      if (put["Reading.scalar"].isMarked(true, true)) {
	double tmpval = put["Reading.scalar"].as<double>();
	cur["Reading.scalar"] = tmpval;
	cur["Reading.value"]  = pvxs::shared_array<double>({tmpval}).freeze();
	nowTS("Reading.timeStamp");
	flag = true;
      }

      // Update Setting.value if present in client's request
      if (put["Setting.value"].isMarked(true, true)) {
	cur["Setting.value"] = put["Setting.value"].as<float>();
	nowTS("Setting.timeStamp");
	flag = true;
      }

      // Update Control.value if present in client's request
      if (put["Control.value"].isMarked(true, true)) {
	cur["Control.value"] = put["Control.value"].as<int16_t>();
	nowTS("Control.timeStamp");
	flag = true;
      }

      // Post the new value and acknowledge
      if (flag) {
	pv.post(std::move(cur));
	op->reply();
      }
      else {
	op->error("No recognized writable fields in PUT");
	return;
      }

    }
    catch (const std::exception& e) {
      op->error(e.what());
    }
    catch (...) {
      op->error("unknown error");
    }
  });

  try {
    sharedpv.open(value);
    cout << "Created record " << name << endl;
  }
  catch (const std::exception& e) {
    errlogPrintf("PV::PV('%s'): sharedpv.open() threw: %s\n", name.c_str(), e.what());
    throw;
  }
  catch (...) {
    errlogPrintf("PV::PV('%s'): sharedpv.open() threw unknown exception\n", name.c_str());
    throw;
  }

}

PV::~PV()
{
  try {
    sharedpv.close();
    cout << "Destroyed record " << name << endl;
  }
  catch (const std::exception& e) {
    errlogPrintf("PV::~PV('%s'): sharedpv.close() threw: %s\n", name.c_str(), e.what());
  }
  catch (...) {
    errlogPrintf("PV::~PV('%s'): sharedpv.close() threw unknown exception\n", name.c_str());
  }
}

//************************************************************************//
//* Functions                                                            *//
//************************************************************************//
PV::Ptr PV::create(const string& recordName)
{
  try {
    return std::make_shared<PV>(recordName);
  }
  catch (const std::exception& e) {
    errlogPrintf("PV::create('%s') failed: %s\n", recordName.c_str(), e.what());
    return nullptr;
  }
  catch (...) {
    errlogPrintf("PV::create('%s') failed with unknown exception\n", recordName.c_str());
    return nullptr;
  }
}


//**** readPVs ***********************************************************//
short PV::readPVs(aSubRecord* pvData)
{

  // Get acnetPV name - already checked in CallReadPVs
  char* aPV = (char*)pvData->a;

  // Find acnetPV created by createAcnetDeviceRecord
  auto rec = ACNET::find(aPV);
  if (!rec) {
    errlogPrintf("readPVs: %s not found\n",aPV);
    return -1;
  }

  // Locally, use V to store data
  pvxs::Value& V  = rec->root();
  pvxs::server::SharedPV& publisher = rec->sharedPV();

  try {
    epicsTimeStamp now;
    if (epicsTimeGetCurrent(&now) != 0) {
      now.secPastEpoch = 0;
      now.nsec = 0;
    }

    // Update fields with current values
    // In what follows, "defensive" pointers are applied by reinterpreting pointers safely
    V["Name"] = sstr(aPV);

    if (pvData->nog == 1) {
      V["Description"] = sstr(reinterpret_cast<const char*>(pvData->h));
      V["EGU"] = sstr(reinterpret_cast<const char*>(pvData->i));
    }
    else {
      V["Description"] = sstr(reinterpret_cast<const char*>(pvData->j));
      V["EGU"] = sstr(reinterpret_cast<const char*>(pvData->k));
    }

    // Basic Status
    if (pvData->b) {
      V["BasicStatus.value"] = *reinterpret_cast<const int16_t*>(pvData->b);
    }
    else {
      V["BasicStatus.value"] = static_cast<const int16_t>(0);
    }
    writeTS(V, "BasicStatus.timeStamp", now.secPastEpoch, now.nsec);

    // Extended Status
    if (pvData->c) {
      V["ExtendedStatus.value"] = *reinterpret_cast<const int32_t*>(pvData->c);
    }
    else {
      V["ExtendedStatus.value"] = static_cast<const int32_t>(0);
    }
    writeTS(V, "ExtendedStatus.timeStamp", now.secPastEpoch, now.nsec);

    // Digital Status
    if (pvData->d) {
      V["DigitalStatus.value"] = *reinterpret_cast<const int16_t*>(pvData->d);
    }
    else {
      V["DigitalStatus.value"] = static_cast<const int16_t>(0);
    }
    writeTS(V, "DigitalStatus.timeStamp", now.secPastEpoch, now.nsec);

    // Analog Status
    if (pvData->e) {
      V["AnalogStatus.value"] = *reinterpret_cast<const float*>(pvData->e);
    }
    else {
      V["AnalogStatus.value"] = 0.0f;
    }
    writeTS(V, "AnalogStatus.timeStamp", now.secPastEpoch, now.nsec);

    // Reading
    // Reading.value is either 1 element from f or an array from g
    // If pvData->nog>1, then reading is a waveform
    shared_array<double> arr;
    if (!pvData->nog) {
      errlogPrintf("readPVs: for %s, pvData->g is null while nog=%u; using empty array\n",
		   pvData->name, pvData->nog);
      arr = shared_array<double>();
    }

    // Waveform data read
    else if (pvData->nog > 1) {
      const size_t maxsize = clampCount(pvData->nog);
      arr = pvxs::shared_array<double>(maxsize);
      const double* wfdata = pvData->g ? reinterpret_cast<const double*>(pvData->g) : nullptr;
      for (size_t i = 0u; i < maxsize; ++i) {
	arr.data()[i] = wfdata ? wfdata[i] : 0.0;
      }
    }

    // Single value read
    else {
      if (pvData->f) {
	arr = shared_array<double>({*reinterpret_cast<const double*>(pvData->f)});
	V["Reading.scalar"] = *reinterpret_cast<const double*>(pvData->f);
      }
      else {
	arr = shared_array<double>({0.0});
      }
    }
    V["Reading.value"]  = arr.freeze();
    writeTS(V, "Reading.timeStamp", now.secPastEpoch, now.nsec);
   
    // Reading is finished, now publish data to acnetPV
    try {
      publisher.post(V);
    }
    catch (const std::exception& e) {
      errlogPrintf("readPVs('%s'): sharedpv.post() failed: %s\n", pvData->name, e.what());
      return -1;
    }

    // Output to Setting or Control
    try {

      // Check for a setting or control
      string sspos = ((char*)pvData->l); int16_t spos = (int16_t)(sspos.find("dummy"));
      string scpos = ((char*)pvData->m); int16_t cpos = (int16_t)(scpos.find("dummy"));

      // Setting
      if (spos==-1 && cpos>=0) {
	auto arr = V["Reading.value"].as<pvxs::shared_array<const double>>();
	*(double*)pvData->vala = static_cast<double>(arr[0]);
      }
      
      // Control
      else if (spos>=0 && cpos==-1) {
	*(short*)pvData->valb = V["BasicStatus.value"].as<int16_t>();
      }

      // Both
      else if (spos==-1 && cpos==-1) {
	auto arr = V["Reading.value"].as<pvxs::shared_array<const double>>();
	*(double*)pvData->vala = static_cast<double>(arr[0]);
	epicsThreadSleep(0.1);
	*(short*)pvData->valb = V["BasicStatus.value"].as<int16_t>();
      }

      // All finished
      return 0;
    }
    catch (const std::exception& e) {
      errlogPrintf("readPVs: failed to Set/Ctrl from PV=%s, error: %s\n",
		   pvData->name, e.what());
      return -1;
    }
 
  }
  catch (const std::exception& e) {
    errlogPrintf("readPVs('%s') error: %s\n", pvData->name, e.what());
  }
  catch (...) {
    errlogPrintf("readPVs('%s') unknown error\n", pvData->name);
  }
  return -1;
}


//***** setPVs ***********************************************************//
short PV::setPVs(aSubRecord* pvData)
{
  // Get acnetPV name
  char* aPV = (char*)pvData->a;

  // Find acnetPV created by createAcnetDeviceRecord (checked by caller)
  auto rec = ACNET::find(aPV);

  // Get current value and compare to old value
  try {
    Value cur = this->sharedpv.fetch();
    float setv = cur["Setting.value"].as<float>();
    if (setv != oldSetting) {
      *(float*)pvData->vala = setv;
      *(float*)pvData->valb = oldSetting;
      oldSetting = setv;

      // Keep cache aligned
      this->value = std::move(cur);
    }
  }
  catch (const std::exception& e) {
    errlogPrintf("setPVs('%s') error: %s\n", pvData->name, e.what());
  }
  catch (...) {
    cerr << "setPVs error" << endl;
    return -1;
  }
  return 0;
}


//***** ctrlPVs ***********************************************************//
short PV::ctrlPVs(aSubRecord* pvData)
{
  // Get acnetPV name
  char* aPV = (char*)pvData->a;

  // Find acnetPV created by createAcnetDeviceRecord (checked by caller)
  auto rec = ACNET::find(aPV);

  try {
    Value cur = this->sharedpv.fetch();
    int16_t ctrl = cur["Control.value"].as<int16_t>();
    if (ctrl != oldControl) {
      *(int16_t*)pvData->vala = ctrl;
      *(int16_t*)pvData->valb = oldControl;
      oldControl = ctrl;

      // Keep cache aligned
      this->value = std::move(cur);
    }
  }
  catch (const std::exception& e) {
    errlogPrintf("ctrlPVs('%s') error: %s\n", pvData->name, e.what());
  }
  catch (...) {
    cerr << "ctrlPVs error" << endl;
    return -1;
  }
  return 0;
}


//***** Clean Up **********************************************************//
void PV::unpublish() noexcept {
  try { sharedpv.close() ;}
  catch(...) {}
}


//************************************************************************//
//* Ensure that worker functions are initialized at the right time       *//
//************************************************************************//
namespace ACNET {
  void ReadPVs()    {}
  void SetPVs()     {}
  void CtrlPVs()     {}
  void CleanupPVs() {}
}

static void acnetReadPVsHook(initHookState st)
{
  if (st == initHookAfterCallbackInit) { ACNET::ReadPVs(); }
}
static void acnetSetPVsHook(initHookState st)
{
  if (st == initHookAfterCallbackInit) { ACNET::SetPVs(); }
}
static void acnetCtrlPVsHook(initHookState st)
{
  if (st == initHookAfterCallbackInit) { ACNET::CtrlPVs(); }
}
static void acnetCleanupPVsHook(initHookState st)
{
  if (st == initHookAtEnd) { ACNET::CleanupPVs(); }
}
void acnetPVsInitHookRegistrar(void)
{
  initHookRegister(&acnetReadPVsHook);
  initHookRegister(&acnetSetPVsHook);
  initHookRegister(&acnetCtrlPVsHook);
  initHookRegister(&acnetCleanupPVsHook);
}

//************************************************************************//
//* Register Functions                                                   *//
//************************************************************************//
extern "C" {

short CallReadPVs(aSubRecord* pvData)
{
  if (!pvData) {
    errlogPrintf("CallReadPVs: pvData is null\n");
    return -1;
  }

  // Resolve acnetPV target name
  char* aPV = (char*)pvData->a;
  if (!aPV || !*aPV) {
    errlogPrintf("CallReadPVs: A is empty\n");
    return -1;
  }

  // Find acnetPV created by createAcnetDeviceRecord
  auto pv = ACNET::find(aPV);
  if (!pv) {
    errlogPrintf("CallReadPVs: %s not found\n",aPV);
    return -1;
  }

  try {
    return pv->readPVs(pvData);
  }

  catch (const std::exception& e) {
    errlogPrintf("CallReadPVs('%s') failed: %s\n", pvData->name, e.what());
  }
  catch (...) {
    errlogPrintf("CallReadPVs('%s') failed: unknown exception\n", pvData->name);
  }
  return -1;
}

short CallSetPVs(aSubRecord* pvData)
{
  if (!pvData) {
    errlogPrintf("CallSetPVs: pvData is null\n");
    return -1;
  }

  // Resolve acnetPV target name
  char* aPV = (char*)pvData->a;
  if (!aPV || !*aPV) {
    errlogPrintf("CallSetPVs: A is empty\n");
    return -1;
  }

  // Find acnetPV created by createAcnetDeviceRecord
  auto pv = ACNET::find(aPV);
  if (!pv) {
    errlogPrintf("CallSetPVs: %s not found\n",aPV);
    return -1;
  }

  try {
    return pv->setPVs(pvData);
  }
  
  catch (const std::exception& e) {
    errlogPrintf("CallSetPVs('%s') failed: %s\n", pvData->name, e.what());
  }
  catch (...) {
    errlogPrintf("CallSetPVs('%s') failed: unknown exception\n", pvData->name);
  }
  return -1;
}

short CallCtrlPVs(aSubRecord* pvData)
{
  if (!pvData) {
    errlogPrintf("CallCtrlPVs: pvData is null\n");
    return -1;
  }

  // Resolve acnetPV target name
  char* aPV = (char*)pvData->a;
  if (!aPV || !*aPV) {
    errlogPrintf("CallCtrlPVs: A is empty\n");
    return -1;
  }

  // Find acnetPV created by createAcnetDeviceRecord
  auto pv = ACNET::find(aPV);
  if (!pv) {
    errlogPrintf("CallCtrlPVs: %s not found\n",aPV);
    return -1;
  }

  try {
    return pv->ctrlPVs(pvData);
  }
  
  catch (const std::exception& e) {
    errlogPrintf("CallCtrlPVs('%s') failed: %s\n", pvData->name, e.what());
  }
  catch (...) {
    errlogPrintf("CallCtrlPVs('%s') failed: unknown exception\n", pvData->name);
  }
  return -1;
}

short CallCleanupPVs(aSubRecord* pvData) {

  if (!pvData) {
    errlogPrintf("CallCleanupPVs: pvData is null\n");
    return -1;
  }

  try {
    auto pv = static_cast<PV*>(pvData->dpvt);
    if (pv) {
      delete pv;   // This calls ~PV()
      pvData->dpvt = nullptr;
    }
    return 0;
  }

  catch (const std::exception& e) {
    errlogPrintf("CallCleanupPVs('%s') failed: %s\n", pvData->name, e.what());
  }
  catch (...) {
    errlogPrintf("CallCleanupPVs('%s') failed: unknown exception\n", pvData->name);
  }
  return -1;
}

epicsRegisterFunction(CallReadPVs);
epicsRegisterFunction(CallSetPVs);
epicsRegisterFunction(CallCtrlPVs);
epicsRegisterFunction(CallCleanupPVs);

epicsExportRegistrar(acnetPVsInitHookRegistrar);
}
