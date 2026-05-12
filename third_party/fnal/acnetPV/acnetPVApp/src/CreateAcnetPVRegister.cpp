// CreateAcnetPVRegister.cpp
#include "acnetPVXS.h"

using pvxs::server::StaticSource;
// ------------------------------------------------------------------
// Provide (or import) a StaticSource builder to register PVs into.
// If you already have one and add it to a Server elsewhere, just
// replace sourceBuilder() with your accessor or mark it `extern`.
// ------------------------------------------------------------------
static StaticSource& acnetStaticSource()
{
    static StaticSource src = StaticSource::build();
    return src;
}

// Attach to IOC's server in correct sequence
static void acnetAttachHook(initHookState st)
{
  if (st != initHookAfterIocBuilt) return;
  pvxs::ioc::server().addSource("acnet", acnetStaticSource().source());
}

// Keep acnetPVs alive outside scope of these functions
static std::unordered_map<std::string, ACNET::PV::Ptr>&acnetRegistry()
{
  static std::unordered_map<std::string, ACNET::PV::Ptr> reg;
  return reg;
}

// Export a lookup accessor for other TUs
namespace ACNET {
  ACNET::PV::Ptr find(const std::string& name)
  {
    auto& reg = acnetRegistry();
    auto it = reg.find(name);
    return it==reg.end() ? ACNET::PV::Ptr() : it->second;
  }
}

// ---------------- IOC shell command: createAcnetDeviceRecord <name> ------------
static const iocshArg   createArg0   = { "recordName", iocshArgString };
static const iocshArg*  createArgs[] = { &createArg0 };
static const iocshFuncDef createFuncDef = { "createAcnetDeviceRecord", 1, createArgs };
static const iocshFuncDef deleteFuncDef = { "deleteAcnetDeviceRecord", 1, createArgs };

static void createCallFunc(const iocshArgBuf* args)
{
  const char* nameC = args[0].sval;
  if (!nameC || !*nameC) {
    errlogSevPrintf(errlogMajor, "createAcnetDeviceRecord: missing/empty recordName\n");
    return;
  }
  const std::string name{nameC};

  try {
    // Build your wrapper (which owns a pvxs::server::SharedPV)
    auto rec = ACNET::PV::create(name);
    if (!rec) {
      errlogSevPrintf(errlogMajor, "createAcnetDeviceRecord: failed to create '%s'\n", name.c_str());
      return;
    }

    // Add PV
    //pvxs::ioc::server().addPV(nameC, rec->sharedPV());
    rec->registerInto(acnetStaticSource());

    // Keep it alive
    acnetRegistry()[name] = rec;

    /*******************************************************************/
    // Don't recreate a PV
    /*
    auto& reg = acnetRegistry();
    if (reg.count(name)) {
      errlogSevPrintf(errlogMinor, "createAcnetDeviceRecord: %s already exists - skipping\n",
		      name.c_str());
      return;
    }
    */

    // Give it a moment
    epicsThreadSleep(0.2);
    errlogPrintf("createAcnetDeviceRecord: added '%s'\n", name.c_str());
  }
  catch (const std::exception& e) {
    errlogSevPrintf(errlogMajor, "createAcnetDeviceRecord: %s failed: %s\n", name.c_str(), e.what());
  }
  catch (...) {
    errlogSevPrintf(errlogMajor, "createAcnetDeviceRecord: %s failed: unknown exception\n", name.c_str());
  }
}

static void deleteCallFunc(const iocshArgBuf* args)
{
  const char* nameC = args[0].sval;
  if(!nameC || !*nameC) { errlogSevPrintf(errlogMajor, "deleteAcnetDevice: missing name\n"); return; }
  std::string name{nameC};

  auto& reg = acnetRegistry();
  auto it = reg.find(name);
  if (it == reg.end()) {
    errlogSevPrintf(errlogMinor, "deleteAcnetDevice: '%s' not found\n", name.c_str());
    return;
  }

  // Best-effort unpublish: close the SharedPV then drop our ref.
  try {
    it->second->unpublish();   // or expose a helper if sharedPV() is const
  } catch(...) {}

  reg.erase(it);
  errlogPrintf("deleteAcnetDevice: removed '%s'\n", name.c_str());
}



// Registrar
static void acnetCreateCmdRegistrar(void)
{
  iocshRegister(&createFuncDef, &createCallFunc);
  iocshRegister(&deleteFuncDef, &deleteCallFunc);
}
static void acnetAttachRegistrar(void)
{
  initHookRegister(&acnetAttachHook);
}

extern "C" {
  epicsExportRegistrar(acnetCreateCmdRegistrar);
  epicsExportRegistrar(acnetAttachRegistrar);
}
