#ifndef ACNET_PVXS_H
#define ACNET_PVXS_H

#include <pvxs/source.h>
#include <pvxs/sharedpv.h>
#include <pvxs/server.h>
#include <pvxs/iochooks.h>
#include <pvxs/log.h>

#include <registryFunction.h>
#include <epicsExport.h>
#include <epicsTypes.h>
#include <epicsThread.h>
#include <epicsTime.h>
#include <dbAccess.h>
#include <initHooks.h>
#include <aSubRecord.h>
#include <iocsh.h>
#include <errlog.h>

#include <algorithm>
#include <memory>
#include <iostream>
#include <sstream>
#include <string>
#include <cstdint>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include <cmath>
#include <cstring>

namespace ACNET {

class PV {
public:
  using Ptr = std::shared_ptr<PV>;

  // Constructor / Destructor
  explicit PV(const std::string& recordName);
  ~PV();

  // Factory
  static Ptr create(const std::string& recordName);

  // Accessors for aSub
  pvxs::server::SharedPV&       sharedPV()       { return sharedpv; }
  const pvxs::server::SharedPV& sharedPV() const { return sharedpv; }
  pvxs::Value&                  root()           { return value; }
  const pvxs::Value&            root()     const { return value; }

  // Methods to interact with EPICS aSubRecord
  short readPVs(aSubRecord* pvData);
  short setPVs(aSubRecord* pvData);
  short ctrlPVs(aSubRecord* pvData);

  void registerInto(pvxs::server::StaticSource& src) {
    src.add(name, sharedpv);
  }

  void unpublish() noexcept;

private:
  std::string name;
  pvxs::server::SharedPV sharedpv;
  pvxs::Value value;
  int16_t oldControl = -1;
  float   oldSetting = -999.999;

  // Global typedef for ACNET PV structure
  static const pvxs::TypeDef acnetTypeDef;
};

  // Forward declare is optional here since PV is already declared above
  PV::Ptr find(const std::string& name);
} // namespace ACNET

#ifdef __cplusplus
extern "C" {
#endif

// Wrappers for use in EPICS database / aSubRecord
short CallReadPVs(aSubRecord* pvData);
short CallSetPVs(aSubRecord* pvData);
short CallCtrlPVs(aSubRecord* pvData);
short CallCleanupPVs(aSubRecord* pvData);

#ifdef __cplusplus
}
#endif

#endif // ACNET_PVXS_H
