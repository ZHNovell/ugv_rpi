#!/usr/bin/env python3
"""Run YOLO26-depth ONNX models and save depth heatmap visualizations.

Example:
    python onnx_model/infer_depth_onnx.py \
        --model onnx_model/yolo26n-depth.onnx \
        --source ax650/depth8/images/kv1_NYUdata_NYU0532.jpg
"""

from __future__ import annotations

import argparse
import time
from pathlib import Path

import cv2
import numpy as np
import onnxruntime as ort


def parse_args() -> argparse.Namespace:
    """Parse command-line arguments."""
    root = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description="YOLO26-depth ONNX Runtime inference")
    parser.add_argument("--model", type=Path, default=root / "yolo26n-depth.onnx", help="Path to an ONNX model")
    parser.add_argument("--axera", action="store_true", help="Use Axera engine for inference")
    parser.add_argument("--source", type=Path, required=True, help="Path to one input image")
    parser.add_argument("--output-dir", type=Path, default=root / "runs", help="Directory for output files")
    parser.add_argument("--alpha", type=float, default=0.6, help="Heatmap opacity in the overlay, in [0, 1]")
    parser.add_argument("--cmap", choices=("jet", "inferno"), default="jet", help="OpenCV heatmap color map")
    parser.add_argument(
        "--mode",
        choices=("disparity", "metric"),
        default="disparity",
        help="Color normalization: inverse depth or metric depth",
    )
    parser.add_argument(
        "--postprocess",
        choices=("default", "predictor"),
        default="default",
        help="Depth resize path: direct ONNX output or Ultralytics predictor-compatible geometry",
    )
    args = parser.parse_args()
    if not 0.0 <= args.alpha <= 1.0:
        parser.error("--alpha must be in [0, 1]")
    return args

def model_input_size(session, use_axera: bool) -> tuple[str, tuple[int, int]]:
    """Return the image input name and static (height, width) required by the ONNX model."""
    inputs = session.get_inputs()
    if len(inputs) != 1:
        raise ValueError(f"Expected one model input, got {len(inputs)}")

    image_input = inputs[0]
    shape = image_input.shape
    if not isinstance(shape[2], int) or not isinstance(shape[3], int):
        raise ValueError(f"Dynamic ONNX input shapes are not supported by this script: {shape}")
    if use_axera:
        return image_input.name, (shape[1], shape[2])
    else:
        return image_input.name, (shape[2], shape[3])


def letterbox(image: np.ndarray, new_shape: tuple[int, int]) -> tuple[np.ndarray, tuple[int, int, int, int]]:
    """Resize and center-pad an image exactly as Ultralytics LetterBox does for a fixed ONNX input."""
    height, width = image.shape[:2]
    target_height, target_width = new_shape
    gain = min(target_height / height, target_width / width)
    resized_width, resized_height = round(width * gain), round(height * gain)
    pad_width, pad_height = target_width - resized_width, target_height - resized_height
    left, right = round(pad_width / 2 - 0.1), round(pad_width / 2 + 0.1)
    top, bottom = round(pad_height / 2 - 0.1), round(pad_height / 2 + 0.1)

    if (width, height) != (resized_width, resized_height):
        image = cv2.resize(image, (resized_width, resized_height), interpolation=cv2.INTER_LINEAR)
    image = cv2.copyMakeBorder(image, top, bottom, left, right, cv2.BORDER_CONSTANT, value=(114, 114, 114))
    return image, (top, bottom, left, right)


def preprocess(image: np.ndarray, input_size: tuple[int, int], use_axera: bool = False) -> tuple[np.ndarray, tuple[int, int, int, int]]:
    """Letterbox BGR input, convert it to RGB NCHW float32, and normalize to [0, 1]."""
    image, padding = letterbox(image, input_size)
    if use_axera:
        tensor = image[..., ::-1][None]
        return np.ascontiguousarray(tensor, dtype=np.uint8), padding
    else:
        tensor = image[..., ::-1].transpose(2, 0, 1)[None]
        return np.ascontiguousarray(tensor, dtype=np.float32) / 255.0, padding


def output_depth_map(output: np.ndarray) -> np.ndarray:
    """Extract one 2D depth map from a standard ONNX depth output tensor."""
    output = np.asarray(output)
    if output.ndim == 4 and output.shape[:2] == (1, 1):
        return output[0, 0]
    elif output.ndim == 3 and output.shape[0] == 1:
        return output[0]
    elif output.ndim == 2:
        return output
    raise ValueError(f"Expected one depth map with shape (1, 1, H, W), got {output.shape}")


def restore_depth(
    output: np.ndarray,
    original_shape: tuple[int, int],
    padding: tuple[int, int, int, int],
) -> np.ndarray:
    """Remove LetterBox padding and resize the exported ONNX output to the original image size."""
    depth = output_depth_map(output)

    top, bottom, left, right = padding
    end_y = depth.shape[0] - bottom if bottom else depth.shape[0]
    end_x = depth.shape[1] - right if right else depth.shape[1]
    depth = depth[top:end_y, left:end_x]
    if depth.size == 0:
        raise ValueError("LetterBox crop is empty; model output and input geometry are inconsistent")
    return cv2.resize(depth.astype(np.float32), (original_shape[1], original_shape[0]), interpolation=cv2.INTER_LINEAR)


def restore_depth_predictor_aligned(output: np.ndarray, original_shape: tuple[int, int]) -> np.ndarray:
    """Match ``DepthPredictor.postprocess`` geometry from an exported 4x-upsampled ONNX output.

    The PyTorch model passes its H/4 head map directly to ``ops.scale_masks``. ONNX export upsamples that map by four
    first, so this function reconstructs the H/4 map and applies the same crop-rounding and bilinear resize sequence.
    It reduces the otherwise visible interpolation-only difference when comparing ONNX output with ``YOLO.predict()``.
    """
    depth = output_depth_map(output)
    height, width = depth.shape
    if height % 4 or width % 4:
        raise ValueError(f"Expected an exported depth output divisible by 4, got {depth.shape}")
    head = cv2.resize(depth, (width // 4, height // 4), interpolation=cv2.INTER_LINEAR)
    original_height, original_width = original_shape
    gain = min(head.shape[0] / original_height, head.shape[1] / original_width)
    pad_width = (head.shape[1] - round(original_width * gain)) / 2
    pad_height = (head.shape[0] - round(original_height * gain)) / 2
    top, left = round(pad_height - 0.1), round(pad_width - 0.1)
    bottom = head.shape[0] - round(pad_height + 0.1)
    right = head.shape[1] - round(pad_width + 0.1)
    head = head[top:bottom, left:right]
    if head.size == 0:
        raise ValueError("Predictor-compatible crop is empty; model output and input geometry are inconsistent")
    return cv2.resize(head.astype(np.float32), (original_width, original_height), interpolation=cv2.INTER_LINEAR)


def colorize_depth(depth: np.ndarray, cmap: str, mode: str) -> np.ndarray:
    """Create the Ultralytics-style BGR depth heatmap; invalid depths are black."""
    depth = np.asarray(depth, dtype=np.float32)
    valid = np.isfinite(depth) & (depth > 0)
    values = np.where(valid, 1.0 / np.where(valid, depth, 1.0), 0.0) if mode == "disparity" else depth
    pool = values[valid]
    if pool.size:
        low, high = np.percentile(pool, (2, 98)) if mode == "disparity" else (pool.min(), pool.max())
    else:
        low, high = 0.0, 1.0
    high = max(float(high), float(low) + 1e-6)
    normalized = np.clip((values - low) / (high - low), 0.0, 1.0)
    color_map = {"jet": cv2.COLORMAP_JET, "inferno": cv2.COLORMAP_INFERNO}[cmap]
    heatmap = cv2.applyColorMap((normalized * 255).astype(np.uint8), color_map)
    heatmap[~valid] = 0
    return heatmap


def save_results(
    source: Path,
    output_dir: Path,
    image: np.ndarray,
    depth: np.ndarray,
    heatmap: np.ndarray,
    alpha: float,
) -> tuple[Path, Path, Path]:
    """Save metric depth values, a standalone heatmap, and a heatmap overlay."""
    output_dir.mkdir(parents=True, exist_ok=True)
    stem = source.stem
    depth_path = output_dir / f"{stem}_depth.npy"
    heatmap_path = output_dir / f"{stem}_depth_heatmap.png"
    overlay_path = output_dir / f"{stem}_depth_overlay.jpg"
    image_heatmap = np.hstack((image, heatmap))
    np.save(depth_path, depth)
    if not cv2.imwrite(str(heatmap_path), image_heatmap):
        raise IOError(f"Failed to write {heatmap_path}")
    overlay = cv2.addWeighted(image, 1.0 - alpha, heatmap, alpha, 0)
    if not cv2.imwrite(str(overlay_path), overlay):
        raise IOError(f"Failed to write {overlay_path}")
    return depth_path, heatmap_path, overlay_path


def main() -> None:
    """Run one image through a YOLO26-depth ONNX model."""
    args = parse_args()
    if not args.model.is_file():
        raise FileNotFoundError(f"ONNX model not found: {args.model}")
    image = cv2.imread(str(args.source))
    if image is None:
        raise FileNotFoundError(f"Unable to read image: {args.source}")

    if args.axera:
        import axengine as axe
        session = axe.InferenceSession(str(args.model))
    else:
        session = ort.InferenceSession(str(args.model))
    input_name, input_size = model_input_size(session, args.axera)
    input_tensor, padding = preprocess(image, input_size, args.axera)

    start = time.perf_counter()
    output = session.run(None, {input_name: input_tensor})[0]
    inference_ms = (time.perf_counter() - start) * 1000
    if args.postprocess == "predictor":
        depth = restore_depth_predictor_aligned(output, image.shape[:2])
    else:
        depth = restore_depth(output, image.shape[:2], padding)
    heatmap = colorize_depth(depth, args.cmap, args.mode)
    depth_path, heatmap_path, overlay_path = save_results(
        args.source, args.output_dir, image, depth, heatmap, args.alpha
    )

    valid = depth[np.isfinite(depth) & (depth > 0)]
    print(f"Providers: {session.get_providers()}")
    print(f"Input: {args.source} ({image.shape[1]}x{image.shape[0]}) -> {input_size[1]}x{input_size[0]}")
    print(f"Postprocess: {args.postprocess}")
    print(f"Inference: {inference_ms:.1f} ms")
    print(f"Depth (m): min={valid.min():.3f}, median={np.median(valid):.3f}, max={valid.max():.3f}")
    print(f"Saved depth: {depth_path}")
    print(f"Saved heatmap: {heatmap_path}")
    print(f"Saved overlay: {overlay_path}")


if __name__ == "__main__":
    main()
