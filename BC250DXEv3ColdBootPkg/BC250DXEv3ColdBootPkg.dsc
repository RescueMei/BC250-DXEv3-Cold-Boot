[Defines]
  # Platform identity, output location, supported architecture, and targets.
  PLATFORM_NAME                  = MeiMeiDXEv3_ColdBoot
  PLATFORM_GUID                  = a3a0d7c4-018d-45fe-b378-58464a4341dc
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x0001001A
  OUTPUT_DIRECTORY               = Build/MeiMeiDXEv3_ColdBoot
  SUPPORTED_ARCHITECTURES        = X64
  BUILD_TARGETS                  = DEBUG|RELEASE
  SKUID_IDENTIFIER               = DEFAULT

[Packages]
  # Package declarations that provide types, protocols, library instances, and
  # the local public header used by the platform module.
  MdePkg/MdePkg.dec
  BC250DXEv3ColdBootPkg/BC250DXEv3ColdBootPkg.dec

[LibraryClasses]
  # Platform-specific library implementations selected for the module's
  # abstract EDK II library dependencies.
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  DebugPrintErrorLevelLib|MdePkg/Library/BaseDebugPrintErrorLevelLib/BaseDebugPrintErrorLevelLib.inf
  DebugLib|MdePkg/Library/BaseDebugLibSerialPort/BaseDebugLibSerialPort.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  IoLib|MdePkg/Library/BaseIoLibIntrinsic/BaseIoLibIntrinsic.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  RegisterFilterLib|MdePkg/Library/RegisterFilterLibNull/RegisterFilterLibNull.inf
  SerialPortLib|MdePkg/Library/BaseSerialPortLibNull/BaseSerialPortLibNull.inf
  StackCheckLib|MdePkg/Library/StackCheckLibNull/StackCheckLibNull.inf
  TimerLib|MdePkg/Library/BaseTimerLibNullTemplate/BaseTimerLibNullTemplate.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  UefiDriverEntryPoint|MdePkg/Library/UefiDriverEntryPoint/UefiDriverEntryPoint.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf

[Components]
  # DXE driver included in this platform build.
  BC250DXEv3ColdBootPkg/BC250DXEv3ColdBootDxe/BC250DXEv3ColdBootDxe.inf