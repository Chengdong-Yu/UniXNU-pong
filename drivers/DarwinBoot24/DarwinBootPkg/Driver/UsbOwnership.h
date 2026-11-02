// SPDX-License-Identifier: BSD-2-Clause-Patent
// Optional Pong USB ownership, independent of the existing audio carveout.
// Never certify PHY/repeater retention from a clock bit or StopController.
#include <Protocol/UsbFunctionIo.h>
STATIC UINTN mUsbDmaOffset=MAX_UINTN,mUsbHandoffOffset=MAX_UINTN,mUsbStatusOffset=MAX_UINTN;
STATIC EFI_USBFN_IO_PROTOCOL *mUsbFunction;
STATIC BOOLEAN mUsbStopped;
STATIC BOOLEAN UsbRequested (CONST CHAR8 *Args)
{
  CONST CHAR8 *At=Args;
  while((At=AsciiStrStr(At,"pong_usb=1"))!=NULL) {
    if((At==Args || At[-1]==' ' || At[-1]=='\t') &&
       (At[10]==0 || At[10]==' ' || At[10]=='\t'))return TRUE;
    At+=10;
  }
  return FALSE;
}

STATIC BOOLEAN
FindUsbChosen (CONST UINT8 *Data, UINTN Size, UINTN *At, UINTN Depth, UINTN *Chosen)
{
  UINTN Start=*At, I; UINT32 Props, Children, Length; BOOLEAN IsChosen=FALSE;
  if (Depth>64 || *At>Size || Size-*At<8) return FALSE;
  Props=U32(Data+*At); Children=U32(Data+*At+4); *At+=8;
  if (Props>(Size-*At)/36) return FALSE;
  for(I=0;I<Props;I++) {
    CONST CHAR8 *Key;
    if (*At>Size || Size-*At<36 || ScanMem8(Data+*At,32,0)==NULL) return FALSE;
    Key=(CONST CHAR8 *)(Data+*At); Length=U32(Data+*At+32); *At+=36;
    if(Length>Size-*At) return FALSE;
    if(Depth==1 && AsciiStrCmp(Key,"name")==0 && Length==7 && CompareMem(Data+*At,"chosen",7)==0)IsChosen=TRUE;
    if(AsciiStrCmp(Key,"pong-usb-dma")==0 || AsciiStrCmp(Key,"pong-usb-handoff")==0 || AsciiStrCmp(Key,"pong-usb-status")==0)return FALSE;
    *At=ALIGN_VALUE(*At+Length,4);if(*At>Size)return FALSE;
  }
  if(IsChosen){if(*Chosen!=MAX_UINTN)return FALSE;*Chosen=Start;}
  for(I=0;I<Children;I++)if(!FindUsbChosen(Data,Size,At,Depth+1,Chosen))return FALSE;
  return TRUE;
}
STATIC VOID UsbSlot (UINT8 *Data, UINTN *At, CONST CHAR8 *Name, UINT32 Bytes, UINTN *Slot)
{
  AsciiStrCpyS((CHAR8 *)(Data+*At),32,Name);
  WriteUnaligned32((UINT32 *)(Data+*At+32),Bytes);*At+=36;
  *Slot=*At;*At+=Bytes;
}
STATIC VOID AddUsbSlots (VOID **Tree, UINTN *Size)
{
  UINTN At=0,Chosen=MAX_UINTN,Insert; UINT8 *Next;
  mUsbDmaOffset=mUsbHandoffOffset=mUsbStatusOffset=MAX_UINTN;
  if(!FindUsbChosen(*Tree,*Size,&At,0,&Chosen) || At!=*Size || Chosen==MAX_UINTN || *Size>SIZE_16MB-172)return;
  Next=AllocateZeroPool(*Size+172);if(Next==NULL)return;
  Insert=Chosen+8;CopyMem(Next,*Tree,Insert);At=Insert;
  UsbSlot(Next,&At,"pong-usb-dma",16,&mUsbDmaOffset);
  UsbSlot(Next,&At,"pong-usb-handoff",32,&mUsbHandoffOffset);
  UsbSlot(Next,&At,"pong-usb-status",16,&mUsbStatusOffset);
  CopyMem(Next+At,(UINT8 *)*Tree+Insert,*Size-Insert);
  WriteUnaligned32((UINT32 *)(Next+Chosen),U32(Next+Chosen)+3);
  FreePool(*Tree);*Tree=Next;*Size+=172;
}
STATIC VOID QuiesceUsb (UINT64 Tree, EFI_PHYSICAL_ADDRESS Dma)
{
  EFI_STATUS Result; UINT64 *Record;
  mUsbFunction=NULL;mUsbStopped=FALSE;
  if(!Dma || mUsbHandoffOffset==MAX_UINTN || mUsbStatusOffset==MAX_UINTN)return;
  Result=gBS->LocateProtocol(&gEfiUsbFunctionIoProtocolGuid,NULL,(VOID **)&mUsbFunction);
  Record=(UINT64 *)(UINTN)(Tree+mUsbStatusOffset);
  WriteUnaligned64(Record,1);WriteUnaligned64(Record+1,Result);
  if(EFI_ERROR(Result) || mUsbFunction==NULL)return;
  if(mUsbFunction->Revision<EFI_USBFN_IO_PROTOCOL_REVISION) {
    WriteUnaligned64(Record+1,EFI_UNSUPPORTED);return;
  }
  if(mUsbFunction->StopController==NULL)return;
  Result=mUsbFunction->StopController(mUsbFunction);
  WriteUnaligned64(Record,2);WriteUnaligned64(Record+1,Result);
  if(EFI_ERROR(Result))return;
  mUsbStopped=TRUE;
  Record=(UINT64 *)(UINTN)(Tree+mUsbHandoffOffset);
  WriteUnaligned64(Record,1);WriteUnaligned64(Record+1,0xa600000);
  WriteUnaligned64(Record+2,0);WriteUnaligned64(Record+3,2);
  // Bit 1 proves old firmware ownership ended. Bit 0 stays CLEAR: a
  // successful stop does not prove Pong's eUSB2 PHY/repeater remains powered.
  // XNU must not enter MMIO until its board layer can establish that fact.
}
