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

W, H = 640, 480
all_h264 = bytearray()

# ============ Пересоздаём энкодер каждые 4 кадра ============
for batch in range(3):  # 3 батча по 4 кадра = 12 кадров
    print(f"\n=== Batch {batch} ===")

    # Создаём энкодер
    encoder = lib_venc.VideoEncCreate(VENC_CODEC_H264)
    config = VencBaseConfig()
    config.bEncH264Nalu = 1
    config.nInputWidth = W; config.nInputHeight = H
    config.nDstWidth = W; config.nDstHeight = H
    config.nStride = W
    config.eInputFormat = VENC_PIXEL_YUV420SP
    config.memops = mem_ops; config.veOpsS = ve_ops; config.pVeOpsSelf = None
    config.bOnlyWbFlag = 0

    lib_venc.VideoEncInit(encoder, byref(config))

    buf_param = VencAllocateBufferParam()
    buf_param.nBufferNum = 4
    buf_param.nSizeY = W * H
    buf_param.nSizeC = W * H // 2
    lib_venc.AllocInputBuffer(encoder, byref(buf_param))

    # Кодируем 4 кадра
    for i in range(4):
        frame_idx = batch * 4 + i

        input_buf = VencInputBuffer()
        ret = lib_venc.GetOneAllocInputBuffer(encoder, byref(input_buf))
        if ret != 0:
            print(f"Frame {frame_idx}: GetOneAllocInputBuffer FAIL ({ret})")
            break

        img_bgr = np.zeros((H, W, 3), dtype=np.uint8)
        img_bgr[:, :, 0] = (frame_idx * 25) % 256
        img_bgr[:, :, 1] = 128
        img_bgr[:, :, 2] = 128

        img_nv12 = fill_nv12(img_bgr, W, H)
        ctypes.memmove(input_buf.pAddrVirY, img_nv12.ctypes.data, img_nv12.nbytes)
        lib_venc.FlushCacheAllocInputBuffer(encoder, byref(input_buf))

        ret = lib_venc.AddOneInputBuffer(encoder, byref(input_buf))
        if ret != 0:
            print(f"Frame {frame_idx}: AddOneInputBuffer FAIL ({ret})")
            break

        ret = lib_venc.VideoEncodeOneFrame(encoder)
        if ret != 0:
            print(f"Frame {frame_idx}: VideoEncodeOneFrame FAIL ({ret})")
            break

        time.sleep(0.05)

        out_buf = VencOutputBuffer()
        ret = lib_venc.GetOneBitstreamFrame(encoder, byref(out_buf))
        if ret == 0 and out_buf.nSize0 > 0 and out_buf.pData0:
            data = ctypes.string_at(out_buf.pData0, out_buf.nSize0)
            all_h264 += data
            print(f"Frame {frame_idx}: {out_buf.nSize0} bytes")

        lib_venc.ReturnOneAllocInputBuffer(encoder, byref(input_buf))

    # Уничтожаем энкодер
    lib_venc.ReleaseAllocInputBuffer(encoder)
    lib_venc.VideoEncDestroy(encoder)

with open('/tmp/test_vpu_recreate.h264', 'wb') as f:
    f.write(all_h264)
print(f"\nTotal: {len(all_h264)} bytes")
