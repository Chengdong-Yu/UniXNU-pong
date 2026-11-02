/* SPDX-License-Identifier: MIT
 * MobileGestalt question spellings and their native obfuscated aliases.
 * Generated from the MGCopyAnswer key ABI; no image hashes are involved.
 */
static CFStringRef canonical_key(CFStringRef key) {
 if(!key)return key;
 const struct {CFStringRef alias,key;} aliases[]={
  {CFSTR("TF31PAB6aO8KAbPyNKSxKA"),CFSTR("UniqueChipID")},
  {CFSTR("re6Zb+zwFKJNlkQTUeT+/w"),CFSTR("UniqueDeviceID")},
  {CFSTR("VasUgeSzVyHdB27g2XpN0g"),CFSTR("SerialNumber")},
  {CFSTR("566JrJVMlDfnslGpwUzNlQ"),CFSTR("ChipID")},
  {CFSTR("D0cJ8r7U5zve6uA6QbOiLA"),CFSTR("ModelNumber")},
  {CFSTR("zHeENZu+wbg7PUprwNwBWg"),CFSTR("RegionInfo")},
  {CFSTR("mjzObL3exJ2SWJHzmIcrBw"),CFSTR("ModelName")},
  {CFSTR("aS/S7hCcobxbRgwdTuv4bw"),CFSTR("MarketingName")},
  {CFSTR("97JDvERpVwO+GHtthIh7hA"),CFSTR("RegulatoryModelNumber")},
  {CFSTR("2/Q5f5zB6Zyq5NLRO8IZzA"),CFSTR("HardwareModel")},
  {CFSTR("pX2TxZTxWKS7QSXZDC/Z6A"),CFSTR("HasBattery")},
  {CFSTR("7oMXPDSgKgHKMOpTkwEvpw"),CFSTR("GasGaugeCapability")},
  {CFSTR("f2DlVMUVcV+MeWs/g2ku+g"),CFSTR("BatteryCurrentCapacity")},
  {CFSTR("sLe8lqXRlqZM74MNUoVcyQ"),CFSTR("BatteryIsCharging")},
  {CFSTR("jgE7MmmkZAG0BiWVqD7bMQ"),CFSTR("BatteryIsFullyCharged")},
  {CFSTR("na6PSBfq05lkAfWkwAwaGg"),CFSTR("ExternalPowerSourceConnected")},
  {CFSTR("KEXgiVU6k/o6FSddpQ3paQ"),CFSTR("ExternalConnected")},
  {CFSTR("NPzjaX07XnS4KcMZ+l8ymA"),CFSTR("ExternalChargeCapability")},
  {CFSTR("diMa2LVhF7qYAg5T+5htRQ"),CFSTR("ExternalChargeCapable")},
  {CFSTR("paR6NqTecAD44x45kzV87g"),CFSTR("AudioPlaybackCapability")},
  {CFSTR("IsJ70crbx4EbG+9YFNxTUA"),CFSTR("TotalDataCapacity")},
  {CFSTR("IaoIYQwaTm3iGrdqziGl/Q"),CFSTR("TotalDataAvailable")},
  {CFSTR("/YYygAofPDbhrwToVsXdeA"),CFSTR("HardwareModel")},
  {CFSTR("HWModelStr"),CFSTR("HardwareModel")},
  {CFSTR("uyejyEdaxNWSRQQwHmXz1A"),CFSTR("DiskUsage")},
  {CFSTR("Q1Ty5w8gxMWHx3p4lQ1fhA"),CFSTR("MLBSerialNumber")},
  {CFSTR("DcQNvnO5Uhpc1jjGIrzyng"),CFSTR("BatterySerialNumber")},
  {CFSTR("gUd367tVKo9iesORCeuKKg"),CFSTR("FrontCameraModuleSerialNumString")},
  {CFSTR("ABuaGInprLgeuiMBmcFSjQ"),CFSTR("BackCameraModuleSerialNumString")},
  {CFSTR("qlcdNkarcGLcbW3HvBhwaA"),CFSTR("FrontFacingCameraModuleSerialNumber")},
  {CFSTR("nWFV2qG2TjSuH7vZFCdi4w"),CFSTR("RearFacingCameraModuleSerialNumber")},
  {CFSTR("YVNo6vlMjhgQ9yGYV8gatw"),CFSTR("PanelSerialNumber")},
  {CFSTR("PxEDp0oOasJ92F/V7YBa8A"),CFSTR("RawPanelSerialNumber")},
  {CFSTR("VGgUE8ENLhWPEjtVcwVzaA"),CFSTR("TotalDiskCapacity")},
 };
 for(unsigned i=0;i<sizeof(aliases)/sizeof(aliases[0]);i++)if(CFEqual(key,aliases[i].alias))return aliases[i].key;
 return key;
}
