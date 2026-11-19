# SPDX-License-Identifier: GPL-2.0-only
"""Face unlock core: camera capture, face detection and recognition.

Frames come from the front camera through libcamera (GStreamer). YuNet finds
the face and its landmarks, SFace turns the aligned face into a 128-value
feature vector, and a face matches when its cosine similarity to one of the
enrolled vectors reaches THRESHOLD.

Enrolled faces live in STORE/<user>.npy, readable by root only.
"""
import os
import threading
import time

import cv2
import numpy as np

import gi
gi.require_version('Gst', '1.0')
from gi.repository import Gst  # noqa: E402

MODELS = '/usr/share/erhai-face'
STORE = '/var/lib/erhai-face'
# SFace's recommended cosine threshold is 0.363; a little stricter here.
THRESHOLD = 0.42
DETECT_SCORE = 0.8
WIDTH, HEIGHT = 1280, 960

Gst.init(None)


def front_camera():
    """libcamera name of the front camera, or None."""
    mon = Gst.DeviceMonitor.new()
    mon.add_filter('Video/Source', None)
    mon.start()
    try:
        for dev in mon.get_devices():
            props = dev.get_properties()
            if props and props.get_string('api.libcamera.Location') == 'Front':
                name = dev.get_display_name()
                rot = props.get_int('api.libcamera.Rotation')
                return name, (rot[1] if rot[0] else 0)
    finally:
        mon.stop()
    return None, 0


class Camera:
    """Front camera frames as BGR arrays. They arrive upright for the
    tablet's landscape orientation; Recognizer.detect() handles the others."""

    def __init__(self):
        name, self.rotation = front_camera()
        if name is None:
            raise RuntimeError('front camera not found')
        self.pipe = Gst.parse_launch(
            'libcamerasrc camera-name="%s" ! video/x-raw,width=%d,height=%d '
            '! videoconvert ! video/x-raw,format=BGR '
            '! appsink name=sink max-buffers=1 drop=true sync=false'
            % (name, WIDTH, HEIGHT))
        self.sink = self.pipe.get_by_name('sink')
        self.lock = threading.Lock()
        self.pipe.set_state(Gst.State.PLAYING)

    def frame(self, timeout=2.0):
        sample = self.sink.emit('try-pull-sample', int(timeout * Gst.SECOND))
        if sample is None:
            return None
        caps = sample.get_caps().get_structure(0)
        w, h = caps.get_value('width'), caps.get_value('height')
        buf = sample.get_buffer()
        ok, info = buf.map(Gst.MapFlags.READ)
        if not ok:
            return None
        try:
            stride = len(info.data) // h
            img = np.frombuffer(info.data, np.uint8).reshape(h, stride)
            img = img[:, :w * 3].reshape(h, w, 3).copy()
        finally:
            buf.unmap(info)
        return img

    def close(self):
        self.pipe.set_state(Gst.State.NULL)


class Recognizer:
    def __init__(self):
        self.det = cv2.FaceDetectorYN.create(
            os.path.join(MODELS, 'face_detection_yunet_2023mar.onnx'), '',
            (320, 320), DETECT_SCORE, 0.3, 5000)
        self.rot = None
        self.misses = 0
        self.rec = cv2.FaceRecognizerSF.create(
            os.path.join(MODELS, 'face_recognition_sface_2021dec.onnx'), '')

    def detect(self, img):
        """Largest face in img: (rotated image, face row), trying the four
        orientations the tablet can be held in; (None, None) without one."""
        # The orientation of the last face first; the other three only now
        # and then while no face is found, so a lost face stays cheap.
        rots = [self.rot]
        if self.misses % 4 == 0:
            rots += [r for r in (None, cv2.ROTATE_90_CLOCKWISE, cv2.ROTATE_180,
                                 cv2.ROTATE_90_COUNTERCLOCKWISE) if r != self.rot]
        for rot in rots:
            im = img if rot is None else cv2.rotate(img, rot)
            # Detect on a reduced copy; the landmarks scale back up.
            scale = 640 / max(im.shape[:2])
            small = cv2.resize(im, None, fx=scale, fy=scale)
            self.det.setInputSize((small.shape[1], small.shape[0]))
            _, faces = self.det.detect(small)
            if faces is not None and len(faces):
                face = max(faces, key=lambda f: f[2] * f[3]).copy()
                face[:14] /= scale
                self.rot, self.misses = rot, 0
                return im, face
        self.misses += 1
        return None, None

    def feature(self, img, face):
        aligned = self.rec.alignCrop(img, face)
        return self.rec.feature(aligned).flatten()

    @staticmethod
    def similarity(a, b):
        return float(np.dot(a, b) / (np.linalg.norm(a) * np.linalg.norm(b)))


def store_path(user):
    if not user or '/' in user or user.startswith('.'):
        raise ValueError('bad user name')
    return os.path.join(STORE, user + '.npy')


def enrolled(user):
    return os.path.exists(store_path(user))


def load(user):
    return np.load(store_path(user))


def save(user, features):
    os.makedirs(STORE, mode=0o711, exist_ok=True)
    path = store_path(user)
    tmp = path + '.tmp'
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, 'wb') as f:
        np.save(f, np.asarray(features, np.float32))
    os.replace(tmp, path)


def remove(user):
    try:
        os.unlink(store_path(user))
    except FileNotFoundError:
        pass


def verify(user, timeout=4.0, log=None):
    """True when the camera sees an enrolled face of user within timeout.
    log, if given, receives one line saying how the check went."""
    known = load(user)
    rec = Recognizer()
    cam = Camera()
    frames = faces = 0
    best = -1.0
    start = time.monotonic()
    try:
        end = start + timeout
        while time.monotonic() < end:
            img = cam.frame(timeout=max(0.1, end - time.monotonic()))
            if img is None:
                continue
            frames += 1
            im, face = rec.detect(img)
            if face is None:
                continue
            faces += 1
            feat = rec.feature(im, face)
            best = max(best, max(rec.similarity(feat, k) for k in known))
            if best >= THRESHOLD:
                break
        return best >= THRESHOLD
    finally:
        cam.close()
        if log:
            log('%s after %.1f s: %d frames, face in %d, best match %.2f (needs %.2f)'
                % ('match' if best >= THRESHOLD else 'no match',
                   time.monotonic() - start, frames, faces, best, THRESHOLD))
