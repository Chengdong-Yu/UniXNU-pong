/* SPDX-License-Identifier: MIT
 * Default compressed playback on SM8475 uses installed Apple software
 * decoders. Native tone clients' explicit Apple hardware decoder requests
 * also need software selection before their deferred first fill. Other
 * manufacturers and errors without an installed decoder remain native.
 */
#include <AudioToolbox/AudioConverter.h>
#include <AudioToolbox/AudioFormat.h>
#include <stdatomic.h>

static bool pong_audio_codec_process(const char *p) {
    return p && (!strcmp(p,"audiomxd") || !strcmp(p,"AudioConverterService") ||
                 !strcmp(p,"AUHostingServiceXPC_arrow") || !strcmp(p,"Preferences") ||
                 !strcmp(p,"tonelibraryd") || !strcmp(p,"SpringBoard"));
}
static OSStatus pong_software_decoder(const AudioStreamBasicDescription *in,
    const AudioStreamBasicDescription *out,AudioConverterRef *converter,OSStatus original,bool preferred) {
    if(!pong_audio_codec_process(getprogname()) || !in || !out || !converter ||
       in->mFormatID==kAudioFormatLinearPCM || out->mFormatID!=kAudioFormatLinearPCM ||
       (!preferred && original!=kAudioConverterErr_FormatNotSupported &&
        original!=kAudioConverterErr_OperationNotSupported &&
        original!=kAudioConverterErr_HardwareInUse &&
        original!=kAudioConverterErr_NoHardwarePermission))return original;
    UInt32 bytes=0;
    if(AudioFormatGetPropertyInfo(kAudioFormatProperty_Decoders,sizeof(in->mFormatID),
       &in->mFormatID,&bytes) || !bytes || bytes>64*sizeof(AudioClassDescription) ||
       bytes%sizeof(AudioClassDescription))return original;
    AudioClassDescription descriptions[64];
    if(AudioFormatGetProperty(kAudioFormatProperty_Decoders,sizeof(in->mFormatID),
       &in->mFormatID,&bytes,descriptions) || bytes>sizeof descriptions ||
       bytes%sizeof(AudioClassDescription))return original;
    UInt32 count=0;
    for(UInt32 i=0;i<bytes/sizeof(descriptions[0]);i++) {
        AudioClassDescription d=descriptions[i];
        if(d.mManufacturer==kAppleSoftwareAudioCodecManufacturer &&
           d.mSubType==in->mFormatID)descriptions[count++]=d;
    }
    if(!count)return original;
    /* dlsym resolves the native implementation; no interposed recursion. */
    OSStatus (*specific)(const AudioStreamBasicDescription *,const AudioStreamBasicDescription *,
        UInt32,const AudioClassDescription *,AudioConverterRef *)=
        dlsym(RTLD_NEXT,"AudioConverterNewSpecific");
    if(!specific)return original;
    AudioConverterRef actual=NULL;
    OSStatus status=specific(in,out,count,descriptions,&actual);
    static atomic_uint reports;
    if(atomic_fetch_add(&reports,1)<8)
        fprintf(stderr,"Pong codec18: %s format=%08x original=%d installed software decoder result=%d\n",
            preferred?"software-first":"native-open-fallback",(unsigned)in->mFormatID,(int)original,(int)status);
    if(!status && actual){*converter=actual;return noErr;}
    if(actual)AudioConverterDispose(actual);
    return original;
}
static OSStatus pong_audio_converter_new(const AudioStreamBasicDescription *in,
    const AudioStreamBasicDescription *out,AudioConverterRef *converter) {
    OSStatus (*native)(const AudioStreamBasicDescription *,const AudioStreamBasicDescription *,AudioConverterRef *)=
        dlsym(RTLD_NEXT,"AudioConverterNew");
    if(!native)return kAudioConverterErr_OperationNotSupported;
    // Hardware AAC converters can defer hardware allocation until the first
    // fill. On Pong choose an actually installed software decoder at creation
    // for the default request; explicit class requests keep their contract.
    AudioConverterRef software=NULL;
    if(converter && !pong_software_decoder(in,out,&software,0,true) && software) {
        *converter=software;return noErr;
    }
    OSStatus status=native(in,out,converter);
    return status?pong_software_decoder(in,out,converter,status,false):status;
}
static OSStatus pong_audio_converter_specific(const AudioStreamBasicDescription *in,
    const AudioStreamBasicDescription *out,UInt32 count,const AudioClassDescription *descriptions,
    AudioConverterRef *converter) {
    OSStatus (*native)(const AudioStreamBasicDescription *,const AudioStreamBasicDescription *,UInt32,
        const AudioClassDescription *,AudioConverterRef *)=dlsym(RTLD_NEXT,"AudioConverterNewSpecific");
    if(!native)return kAudioConverterErr_OperationNotSupported;
    // Native alerts may explicitly ask for Apple's hardware AAC decoder,
    // whose allocation is deferred until FillComplexBuffer. That hardware
    // codec is not implemented on Pong. For these qualified native clients,
    // select an actually installed software decoder before accepting such
    // a deferred hardware handle. Other explicit codec requests stay native.
    const char *process=getprogname();
    bool hardware=pong_audio_codec_process(process) && count && count<=64 && descriptions && in && out;
    for(UInt32 i=0;hardware && i<count;i++)
        hardware=descriptions[i].mManufacturer==kAppleHardwareAudioCodecManufacturer &&
            descriptions[i].mSubType==in->mFormatID;
    if(hardware && converter) {
        AudioConverterRef software=NULL;
        if(!pong_software_decoder(in,out,&software,0,true) && software){*converter=software;return noErr;}
    }
    OSStatus status=native(in,out,count,descriptions,converter);
    return status?pong_software_decoder(in,out,converter,status,false):status;
}
__attribute__((used,section("__DATA,__interpose")))
static const struct {const void *replacement,*original;} pong_audio_codec_interpose[]={
    {(const void *)pong_audio_converter_new,(const void *)AudioConverterNew},
    {(const void *)pong_audio_converter_specific,(const void *)AudioConverterNewSpecific}
};
