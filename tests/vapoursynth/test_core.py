#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0 OR MIT
# Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
"""hdrg.Grade under VapourSynth itself.

The plugin is loaded into a VapourSynth core and fed a 4:2:0 clip with a cut,
a fade-out, a flash and a moving square. Its frames, taken in order, at
random from a new instance and through the core's own parallel requests,
are the same bytes; the output carries the PQ color properties and the T.35
payloads of HDR10+ (HDR10Plus) and ST 2094-50 (HdrgST2094_50). Run by ctest
where `import vapoursynth` works: test_core.py <path to the plugin> <past> <future> (the window
the build has: none without the temporal filters).
"""

import sys

import vapoursynth as vs

FRAMES = 60
WIDTH, HEIGHT = 64, 48


def luma(t):
    """The background level of frame t: a cut at 20, a fade-out from 32, a flash at 50."""
    level = 140.0 if t < 20 else 60.0
    if 32 <= t < 48:
        level *= 0.9 ** (t - 31)
    if t == 50:
        level = 235.0
    return int(round(level))


def source(core):
    clips = []
    for t in range(FRAMES):
        def blank(width, height, y):
            return core.std.BlankClip(width=width, height=height, format=vs.YUV420P8, length=1,
                                      color=[y, 120, 136])

        background = luma(t)
        x = (t * 2) % (WIDTH - 10)
        middle = [blank(10, 10, 235)]
        if x > 0:
            middle.insert(0, blank(x, 10, background))
        if x + 10 < WIDTH:
            middle.append(blank(WIDTH - x - 10, 10, background))
        clips.append(core.std.StackVertical([
            blank(WIDTH, 10, background),
            core.std.StackHorizontal(middle),
            blank(WIDTH, HEIGHT - 20, background),
        ]))
    clip = core.std.Splice(clips)
    return core.std.SetFrameProps(clip, _Matrix=1, _Transfer=1, _Primaries=1, _Range=0)


def planes(frame):
    return [bytes(frame[p]) for p in range(frame.format.num_planes)]


def check(condition, message):
    if not condition:
        print("FAIL: " + message)
        sys.exit(1)


def interpolated(hdr, doubled, order, clip):
    """Twice the clip's rate: 2 n - 1 frames, the same bytes in order, at random and in parallel."""
    check(hdr.num_frames == 2 * FRAMES - 1, "2 n - 1 frames at twice the rate")
    check(hdr.fps == clip.fps * 2, "the output's rate")
    twice = [planes(hdr.get_frame(n)) for n in range(hdr.num_frames)]
    check(hdr.get_frame(1).props["HdrgSynthetic"] == 1, "a frame between")
    check(twice[2 * 10] == order[10], "an input frame's output at twice the rate")
    hdr = doubled()
    for n in [90, 21, 22, 118, 5, 61]:
        check(planes(hdr.get_frame(n)) == twice[n], "frame %d at twice the rate, at random" % n)
    hdr = doubled()
    for n, frame in enumerate(hdr.frames(prefetch=16)):
        check(planes(frame) == twice[n], "frame %d at twice the rate, in parallel" % n)


def main():
    core = vs.core
    core.std.LoadPlugin(sys.argv[1])
    clip = source(core)

    past, future = int(sys.argv[2]), int(sys.argv[3])

    def grade():
        return core.hdrg.Grade(clip, past=past, future=future)

    hdr = grade()
    check(hdr.format.id == vs.YUV420P10.value, "the output is 4:2:0 10-bit")
    check(hdr.num_frames == FRAMES, "one frame out per frame in")

    order = [planes(hdr.get_frame(n)) for n in range(FRAMES)]
    props = hdr.get_frame(0).props
    check(props["_Transfer"] == 16 and props["_Matrix"] == 9 and props["_Primaries"] == 9,
          "PQ over BT.2020")
    check(props["MasteringDisplayMaxLuminance"] > 100.0, "the mastering display")
    plus = bytes(props["HDR10Plus"])
    check(len(plus) == 43 and plus[0] == 1 and plus[1] >> 6 == 1,
          "HDR10+ as FFMS2's HDR10Plus: application version 1, one window")
    curve = bytes(props["HdrgST2094_50"])
    check(len(curve) >= 13 and curve[0] == 0 and curve[1] & 0x40,
          "ST 2094-50's T.35 payload: version 0, an adaptive tone map")

    seeks = [45, 10, 11, 59, 3, 30, 31, 12, 50, 0]
    hdr = grade()
    for n in seeks:
        check(planes(hdr.get_frame(n)) == order[n], "frame %d at random" % n)

    hdr = grade()
    for n, frame in enumerate(hdr.frames(prefetch=16)):
        check(planes(frame) == order[n], "frame %d of the parallel requests" % n)

    # the chroma location: the frames' _ChromaLocation or the argument, LEFT without either
    # (CENTER: the halves' edge is vertical, only the horizontal siting moves it)
    def halves():
        left = core.std.BlankClip(width=32, height=32, format=vs.YUV420P8, length=3,
                                  color=[100, 60, 200])
        right = core.std.BlankClip(width=32, height=32, format=vs.YUV420P8, length=3,
                                   color=[100, 200, 60])
        return core.std.SetFrameProps(core.std.StackHorizontal([left, right]), _Matrix=1,
                                      _Transfer=1, _Primaries=1, _Range=0)

    sited = planes(core.hdrg.Grade(core.std.SetFrameProps(halves(), _ChromaLocation=1)).get_frame(1))
    named = planes(core.hdrg.Grade(halves(), chromaloc=1).get_frame(1))
    plain = planes(core.hdrg.Grade(halves()).get_frame(1))
    check(sited == named, "_ChromaLocation and chromaloc agree")
    check(sited != plain, "the chroma location changes the chroma edge")

    # another output rate: twice the clip's, the frames between interpolated, the same bytes in
    # order, at random and in parallel
    def doubled():
        return core.hdrg.Grade(clip, past=past, future=future, fpsnum=2 * clip.fps.numerator,
                               fpsden=clip.fps.denominator)

    try:
        hdr = doubled()
    except vs.Error:
        hdr = None  # built without hdrg.interp.mc (HDRG_WITH_TEMPORAL)
    if hdr is not None:
        interpolated(hdr, doubled, order, clip)

    rgb = core.resize.Point(clip, format=vs.RGB24, matrix_in_s="709")
    rgb = core.std.SetFrameProps(rgb, _Transfer=13, _Primaries=1)
    out = core.hdrg.Grade(rgb, format="rgb48")
    check(out.format.id == vs.RGB48.value, "rgb48 out of RGB in")
    check(out.get_frame(FRAMES - 1).props["_Primaries"] == 12, "Display P3")

    print("OK: %d frames in order, at random and in parallel" % FRAMES)


if __name__ == "__main__":
    main()
