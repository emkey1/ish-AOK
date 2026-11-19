#!/usr/bin/env ruby
# Wire the Workspace Music player into the Xcode project. Idempotent: safe to
# run repeatedly. Run once now (adds sources + AVFoundation/MediaPlayer); run
# again after deps/audiocodecs-static/build-audiocodecs-apple.sh to link the
# Ogg/Opus codec xcframework once it exists.
#
#   ruby tools/wire_audio_player.rb
#
require "xcodeproj"

PROJECT   = File.expand_path("../iSH-AOK.xcodeproj", __dir__)
SRC_TARGET = "libiSH-AOKApp"                 # where WorkspaceViewController.m compiles
FW_TARGETS = ["AOK-iSH", "AOK-iSH+Linux"]    # where AudioToolbox / liblzma link
XCFRAMEWORK = File.expand_path("../deps/audiocodecs-static/audiocodecs.xcframework", __dir__)

SOURCES = %w[
  AudioPlayerEngine.m
  AudioPCMDecoder.m
  AudioPCMDecoder_AVF.m
  AudioPCMDecoder_Vorbis.m
  AudioPCMDecoder_Opus.m
  AudioLibrary.m
]
HEADERS = %w[
  AudioPlayerEngine.h
  AudioPCMDecoder.h
  AudioPCMDecoder_Internal.h
  AudioLibrary.h
]

proj = Xcodeproj::Project.open(PROJECT)
app_group = proj.files.find { |f| f.display_name == "WorkspaceViewController.m" }.parent

def ref_for(proj, group, name)
  existing = proj.files.find { |f| f.display_name == name }
  return existing if existing
  ref = group.new_reference(name)        # path relative to the "app" group
  ref.last_known_file_type = name.end_with?(".m") ? "sourcecode.c.objc" : "sourcecode.c.h"
  ref
end

# 1) File references (sources + headers) in the app group.
(SOURCES + HEADERS).each { |name| ref_for(proj, app_group, name) }

# 2) Compile the sources in libiSH-AOKApp (same target as WorkspaceViewController.m).
src_target = proj.targets.find { |t| t.name == SRC_TARGET }
SOURCES.each do |name|
  ref = proj.files.find { |f| f.display_name == name }
  already = src_target.source_build_phase.files.any? { |bf| bf.file_ref == ref }
  src_target.source_build_phase.add_file_reference(ref, true) unless already
end

# Build a framework/xcframework reference by hand (add_system_framework can't
# resolve these targets' platform). Mirrors how AudioToolbox.framework is
# already referenced: explicit path + sourceTree.
def linked_ref(proj, name, path, source_tree, file_type)
  existing = proj.files.find { |f| f.display_name == name }
  return existing if existing
  ref = proj.frameworks_group.new_reference(path)
  ref.name = name
  ref.path = path
  ref.source_tree = source_tree
  ref.last_known_file_type = file_type
  ref
end

def link_into_targets(proj, targets, ref)
  targets.each do |tname|
    t = proj.targets.find { |x| x.name == tname }
    next unless t
    next if t.frameworks_build_phase.files.any? { |f| f.file_ref == ref }
    t.frameworks_build_phase.add_file_reference(ref, true)
  end
end

# 2.5) Put the codec headers on the compiler search path for the target that
#      compiles the decoders (linking the xcframework only exposes headers to the
#      linking target). Mirrors deps/liblzma -> libarchive HEADER_SEARCH_PATHS.
#      Both the Headers root AND the opus/ subdir are required: the root resolves
#      <vorbis/vorbisfile.h>, <opus/opusfile.h>, <ogg/ogg.h>, but opusfile.h then
#      does a bare `#include <opus_multistream.h>` that only resolves with the
#      opus/ dir itself on the path.
hdr_paths = ["$(SRCROOT)/deps/audiocodecs-static/Headers",
             "$(SRCROOT)/deps/audiocodecs-static/Headers/opus"]
src_target.build_configurations.each do |c|
  hsp = c.build_settings["HEADER_SEARCH_PATHS"]
  arr = hsp.nil? ? ["$(inherited)"] : (hsp.is_a?(String) ? [hsp] : hsp.dup)
  hdr_paths.each { |p| arr << p unless arr.include?(p) }
  c.build_settings["HEADER_SEARCH_PATHS"] = arr
end

# 3) Link AVFoundation + MediaPlayer in the final app targets.
%w[AVFoundation.framework MediaPlayer.framework].each do |fw|
  ref = linked_ref(proj, fw, "System/Library/Frameworks/#{fw}", "SDKROOT", "wrapper.framework")
  link_into_targets(proj, FW_TARGETS, ref)
end

# 4) Link the codec xcframework — only once it's been built, so the project
#    stays buildable (native formats only) before then.
if File.directory?(XCFRAMEWORK)
  ref = linked_ref(proj, "audiocodecs.xcframework",
                   "deps/audiocodecs-static/audiocodecs.xcframework", "SOURCE_ROOT", "wrapper.xcframework")
  link_into_targets(proj, FW_TARGETS, ref)
  puts "linked audiocodecs.xcframework into #{FW_TARGETS.join(', ')}"
else
  puts "audiocodecs.xcframework not built yet — skipping codec link (native formats still work)."
  puts "  build it with: (cd deps/audiocodecs-static && ./build-audiocodecs-apple.sh) then re-run this script."
end

proj.save

# NOTE: we deliberately do NOT strip IPHONEOS_DEPLOYMENT_TARGET here anymore.
# The deployment target is iOS 15.0 — the minimum the iOS 26+ SDK accepts — set
# authoritatively in app/Project.xcconfig. This block used to delete the
# xcodeproj gem's auto-inserted `IPHONEOS_DEPLOYMENT_TARGET = 15.0` lines (back
# when the project targeted iOS 12), which silently reverted the 15.0 the
# project now actually wants. Stripping removed; 15.0 is correct.

# Verify references resolve to app/<file> and report membership.
puts "\nVerification:"
(SOURCES + HEADERS).each do |name|
  ref = proj.files.find { |f| f.display_name == name }
  puts "  #{name} -> #{ref.real_path.relative_path_from(Pathname.new(File.expand_path('..', __dir__)))}"
end
puts "Sources added to #{SRC_TARGET}; AVFoundation/MediaPlayer linked in #{FW_TARGETS.join(', ')}."
