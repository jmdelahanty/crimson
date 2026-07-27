#include "data_access_cache.h"

namespace crimson::data {

const char *dataCacheAdmissionStatusName(DataCacheAdmissionStatus status) {
  switch (status) {
  case DataCacheAdmissionStatus::Inserted:
    return "inserted";
  case DataCacheAdmissionStatus::Replaced:
    return "replaced";
  case DataCacheAdmissionStatus::RejectedOversize:
    return "rejected_oversize";
  case DataCacheAdmissionStatus::RejectedPressure:
    return "rejected_pressure";
  }
  return "unknown";
}

} // namespace crimson::data
