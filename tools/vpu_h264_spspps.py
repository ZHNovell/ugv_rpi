import ctypes
from ctypes import c_void_p, c_int, c_uint, c_ubyte, c_ulong, c_longlong, Structure, POINTER, byref, CFUNCTYPE
import numpy as np
import cv2
import time

lib_ve = ctypes.CDLL('/usr/lib/aarch64-linux-gnu/libVE.so')
lib_mem = ctypes.CDLL('/usr/lib/aarch64-linux-gnu/libMemAdapter.so')
lib_venc = ctypes.CDLL('/usr/lib/aarch64-linux-gnu/libvencoder.so')

VENC_CODEC_H264 = 0
VENC_PIXEL_YUV420SP = 0
VENC_INDEXPARAM_H264_SPSPPS = 0x101

class VencBaseConfig(Structure):
    _fields_ = [
        ('bEncH264Nalu', c_ubyte),
        ('nInputWidth', c_uint), ('nInputHeight', c_uint),
        ('nDstWidth', c_uint), ('nDstHeight', c_uint),
        ('nStride', c_uint), ('eInputFormat', c_int),
        ('memops', c_void_p), ('veOpsS', c_void_p), ('pVeOpsSelf', c_void_p),
        ('bOnlyWbFlag', c_ubyte),
        ('bLbcLossyComEnFlag2x', c_ubyte), ('bLbcLossyComEnFlag2_5x', c_ubyte),
        ('bIsVbvNoCache', c_ubyte),
        ('bVcuAutoMode', c_uint), ('nFrameNumInGroup', c_uint),
        ('extend_flag', c_uint), ('en_vcu', c_uint),
    ]

class VencHeaderData(Structure):
    _fields_ = [
        ('pBuffer', c_void_p),
        ('nLength', c_uint),
    ]

class VencAllocateBufferParam(Structure):
    _fields_ = [('nBufferNum', c_uint), ('nSizeY', c_uint), ('nSizeC', c_uint)]

class VencRect(Structure):
    _fields_ = [('nLeft', c_int), ('nTop', c_int), ('nWidth', c_int), ('nHeight', c_int)]

class VencInputBuffer(Structure):
    _fields_ = [
        ('nID', c_ulong), ('nPts', c_longlong), ('nFlag', c_uint),
        ('pAddrPhyY', c_void_p), ('pAddrPhyC', c_void_p),
        ('pAddrVirY', c_void_p), ('pAddrVirC', c_void_p),
        ('nWidth', c_int), ('nHeight', c_int), ('nAlign', c_int),
        ('bEnableCorp', c_int), ('sCropInfo', VencRect),
        ('ispPicVar', c_int), ('ispPicVarChroma', c_int),
        ('bUseInputBufferRoi', c_int),
        ('roi_param', c_ubyte * 2048),
        ('bAllocMemSelf', c_int), ('nShareBufFd', c_int),
        ('_pad', c_ubyte * 256),
    ]

class VencOutputBuffer(Structure):
    _fields_ = [
        ('nID', c_int), ('nPts', c_longlong), ('nFlag', c_uint),
        ('nSize0', c_uint), ('nSize1', c_uint),
        ('pData0', c_void_p), ('pData1', c_void_p),
        ('_pad', c_ubyte * 512),
    ]

lib_ve.GetVeOpsS.argtypes = [c_int]; lib_ve.GetVeOpsS.restype = c_void_p
lib_mem.MemAdapterGetOpsS.argtypes = []; lib_mem.MemAdapterGetOpsS.restype = c_void_p
lib_venc.VideoEncCreate.argtypes = [c_int]; lib_venc.VideoEncCreate.restype = c_void_p
lib_venc.VideoEncInit.argtypes = [c_void_p, POINTER(VencBaseConfig)]; lib_venc.VideoEncInit.restype = c_int
lib_venc.VideoEncDestroy.argtypes = [c_void_p]; lib_venc.VideoEncDestroy.restype = None
lib_venc.VideoEncGetParameter.argtypes = [c_void_p, c_int, c_void_p]; lib_venc.VideoEncGetParameter.restype = c_int
lib_venc.AllocInputBuffer.argtypes = [c_void_p, POINTER(VencAllocateBufferParam)]; lib_venc.AllocInputBuffer.restype = c_int
lib_venc.GetOneAllocInputBuffer.argtypes = [c_void_p, POINTER(VencInputBuffer)]; lib_venc.GetOneAllocInputBuffer.restype = c_int
lib_venc.FlushCacheAllocInputBuffer.argtypes = [c_void_p, POINTER(VencInputBuffer)]; lib_venc.FlushCacheAllocInputBuffer.restype = c_int
lib_venc.AddOneInputBuffer.argtypes = [c_void_p, POINTER(VencInputBuffer)]; lib_venc.AddOneInputBuffer.restype = c_int
lib_venc.VideoEncodeOneFrame.argtypes = [c_void_p]; lib_venc.VideoEncodeOneFrame.restype = c_int
lib_venc.GetOneBitstreamFrame.argtypes = [c_void_p, POINTER(VencOutputBuffer)]; lib_venc.GetOneBitstreamFrame.restype = c_int
lib_venc.ReturnOneAllocInputBuffer.argtypes = [c_void_p, POINTER(VencInputBuffer)]; lib_venc.ReturnOneAllocInputBuffer.restype = c_int
lib_venc.ReleaseAllocInputBuffer.argtypes = [c_void_p]; lib_venc.ReleaseAllocInputBuffer.restype = c_int

def get_func_ptr(addr, off):
    return ctypes.c_void_p.from_address(addr + off).value

def fill_nv12(img_bgr, W, H):
    img_yuv = cv2.cvtColor(img_bgr, cv2.COLOR_BGR2YUV_I420)
    img_nv12 = np.zeros((H * 3 // 2, W), dtype=np.uint8)
    img_nv12[:H, :] = img_yuv[:H, :]
    u = img_yuv[H:H + H//4, :].reshape(H//2, W//2)
    v = img_yuv[H + H//4:H + H//2, :].reshape(H//2, W//2)
    uv = np.empty((H//2, W), dtype=np.uint8)
    uv[:, 0::2] = u; uv[:, 1::2] = v
    img_nv12[H:, :] = uv
    return img_nv12

ve_ops = lib_ve.GetVeOpsS(0)
mem_ops = lib_mem.MemAdapterGetOpsS()
open_ptr = get_func_ptr(mem_ops, 0)
open_func = CFUNCTYPE(c_int)(open_ptr)
open_func()

encoder = lib_venc.VideoEncCreate(VENC_CODEC_H264)
W, H = 640, 480
config = VencBaseConfig()
config.bEncH264Nalu = 1
config.nInputWidth = W; config.nInputHeight = H
config.nDstWidth = W; config.nDstHeight = H
config.nStride = W
config.eInputFormat = VENC_PIXEL_YUV420SP
config.memops = mem_ops; config.veOpsS = ve_ops; config.pVeOpsSelf = None
config.bOnlyWbFlag = 0

ret = lib_venc.VideoEncInit(encoder, byref(config))
print(f"VideoEncInit = {ret}")

# ============ Получаем SPS/PPS ============
print("\n=== Get SPS/PPS ===")
header = VencHeaderData()
ret = lib_venc.VideoEncGetParameter(encoder, VENC_INDEXPARAM_H264_SPSPPS, byref(header))
print(f"VideoEncGetParameter = {ret}")
print(f"pBuffer = {hex(header.pBuffer) if header.pBuffer else 'NULL'}")
print(f"nLength = {header.nLength}")

if header.pBuffer and header.nLength > 0:
    sps_pps = ctypes.string_at(header.pBuffer, header.nLength)
    print(f"SPS/PPS hex: {sps_pps.hex()}")
    with open('/tmp/test_spspps.bin', 'wb') as f:
        f.write(sps_pps)
    print("SPS/PPS saved to /tmp/test_spspps.bin")

lib_venc.VideoEncDestroy(encoder)
print("\nDone")
