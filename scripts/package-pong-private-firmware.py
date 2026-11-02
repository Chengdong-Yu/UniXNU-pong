#!/usr/bin/env python3
"""Package the local Pong firmware inputs actually consumed by its builders.

No downloads, device access, image hashes, Apple binaries or credentials.
"""
import argparse
import json
from pathlib import Path
import tarfile
from pong_native_inputs import input_path

ROOT=Path(__file__).resolve().parents[1]
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',required=True,type=Path)
    p.add_argument('--adsp',required=True,type=Path)
    a=p.parse_args();output=a.output.resolve()
    if output.exists():raise RuntimeError('Existing archive retained: '+str(output))
    files=[]
    gpu=ROOT/'out/ipados18/firmware/adreno730'
    for name in ('gmu_gen70000.bin','a730_sqe.fw','a730_zap.mdt','a730_zap.b01','a730_zap.b02'):
        files.append((gpu/name,'adreno730/'+name,'GPU build: GMU/SQE/ZAP'))
    # The current Wi-Fi builder packs every regular input file in this folder.
    wifi=input_path('wifi_firmware')
    for source in sorted(wifi.iterdir()):
        if source.is_file() and not source.is_symlink():files.append((source,'qca6490/'+source.name,'Wi-Fi build: actual QCA6490 firmware/BDF input'))
    files.append((input_path('audio_container'),'audio/tfa98xx.cnt','Audio build: TFA9873 configuration container (NXP, included companion input)'))
    files.append((a.adsp.resolve(),'power/adsp.mbn','Runtime packaging: SM8475 PAS1 ADSP; not embedded in kernel archive'))
    for source,_,_ in files:
        if not source.is_file() or source.is_symlink():raise RuntimeError('Missing local firmware: '+str(source))
    inventory={'device':'Nothing Phone (2) / Pong / SM8475','scope':'Current Darwin24 build and runtime firmware inputs',
        'files':[{'path':name,'bytes':source.stat().st_size,'use':use} for source,name,use in files],
        'apple_content':False,'partition_images':False,'credentials':False,'hashes':False,
        'runtime_qualified':False}
    readme='''# Pong 编译及运行时固件输入

从本机此前由 Pong 原厂 modem/vendor 分区提取的文件打包。包含：
- adreno730：A730 GMU、SQE、split ZAP；构建器默认放入 out/ipados18/firmware/adreno730。
- qca6490：当前 Wi-Fi 构建器使用的全部固件/BDF文件；local.toml 的 pong_build.wifi_firmware 指向解压后的该目录。
- audio/tfa98xx.cnt：NXP TFA9873功放配置容器，属于构建所需的配套厂商输入；pong_build.audio_container 指向该文件。
- power/adsp.mbn：高通 ADSP 运行时固件；用户态构建器通过 --power-firmware 指定 power 目录，装入 /pong/firmware/adsp.mbn。

manifest.json 记录文件大小和用途。没有包含整分区、IPSW、Apple二进制、账号、密码或设备唯一校准分区；QCA固件目录内厂商BDF变体按现有编译输入保留。
未执行镜像哈希或手机回读，未新增固件功能验证。本压缩包与Git仓库分开保存。
'''
    import io
    output.parent.mkdir(parents=True,exist_ok=True)
    with tarfile.open(output,'w:gz',compresslevel=3) as archive:
        for source,name,_ in files:
            meta=archive.gettarinfo(str(source),arcname='pong-private-firmware/'+name)
            meta.uid=meta.gid=0;meta.uname=meta.gname='root';meta.mode=0o644
            with source.open('rb') as f:archive.addfile(meta,f)
        for name,data in [('README.zh.md',readme.encode()),('manifest.json',(json.dumps(inventory,indent=2,ensure_ascii=False)+'\n').encode())]:
            meta=tarfile.TarInfo('pong-private-firmware/'+name);meta.size=len(data);meta.mode=0o644;archive.addfile(meta,io.BytesIO(data))
    print(json.dumps({'archive':str(output),'files':len(files),'input_bytes':sum(x.stat().st_size for x,_,_ in files),'archive_bytes':output.stat().st_size},indent=2))
if __name__=='__main__':main()
