# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 FruitSlicer contributors
#
# Finder picks the app for a file by its identity, then opens any copy with
# that identity. So:
#  - the copy built with FRUIT_OPENS_JOB_FILES (OWNS=ON) is FruitSlicer itself
#    (io.github.anyosag.fruitslicer): it declares the job file type (.fruitjob,
#    "FruitSlicer job") and owns it as an editor, with the document icon macOS
#    draws from the app's;
#  - every other copy (OWNS=OFF: tests, CI, other branches) is
#    io.github.anyosag.fruitslicer.dev and declares no file types at all, so
#    a double-click never lands on it.
#   cmake -DPLIST=<FruitSlicer.app/Contents/Info.plist> -DOWNS=ON|OFF -P check_document_types.cmake
execute_process(COMMAND plutil -convert json -o - "${PLIST}" OUTPUT_VARIABLE json RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "plutil could not read ${PLIST}")
endif()
string(JSON app GET "${json}" CFBundleIdentifier)
set(job io.github.anyosag.fruitslicer.job)
string(JSON n ERROR_VARIABLE no_decl LENGTH "${json}" UTExportedTypeDeclarations)
string(JSON m ERROR_VARIABLE no_docs LENGTH "${json}" CFBundleDocumentTypes)

if(NOT OWNS)
  if(NOT app STREQUAL "io.github.anyosag.fruitslicer.dev")
    message(FATAL_ERROR "a copy built without FRUIT_OPENS_JOB_FILES is ${app}, not io.github.anyosag.fruitslicer.dev")
  endif()
  if(NOT no_decl OR NOT no_docs)
    message(FATAL_ERROR "a copy built without FRUIT_OPENS_JOB_FILES declares file types")
  endif()
  message(STATUS "${app}: no file types, as a copy for tests should be")
  return()
endif()

if(NOT app STREQUAL "io.github.anyosag.fruitslicer")
  message(FATAL_ERROR "the copy built with FRUIT_OPENS_JOB_FILES is ${app}, not io.github.anyosag.fruitslicer")
endif()
if(no_decl)
  message(FATAL_ERROR "the app declares no file types of its own (no UTExportedTypeDeclarations)")
endif()
# The type: its extension and what Finder calls it.
set(declared FALSE)
math(EXPR last "${n} - 1")
foreach(i RANGE ${last})
  string(JSON id GET "${json}" UTExportedTypeDeclarations ${i} UTTypeIdentifier)
  if(id STREQUAL job)
    string(JSON ext GET "${json}" UTExportedTypeDeclarations ${i} UTTypeTagSpecification public.filename-extension 0)
    string(JSON what GET "${json}" UTExportedTypeDeclarations ${i} UTTypeDescription)
    if(NOT ext STREQUAL "fruitjob" OR NOT what STREQUAL "FruitSlicer job")
      message(FATAL_ERROR "${job} is declared for .${ext} as \"${what}\"")
    endif()
    set(declared TRUE)
  endif()
endforeach()
if(NOT declared)
  message(FATAL_ERROR "the app does not declare ${job}")
endif()
# The app owns it, as an editor, with the icon macOS draws from the app's.
if(no_docs)
  message(FATAL_ERROR "the app opens no file types (no CFBundleDocumentTypes)")
endif()
set(owned FALSE)
math(EXPR last "${m} - 1")
foreach(i RANGE ${last})
  string(JSON types ERROR_VARIABLE none GET "${json}" CFBundleDocumentTypes ${i} LSItemContentTypes)
  if(none)
    continue()
  endif()
  string(JSON first GET "${types}" 0)
  if(first STREQUAL job)
    string(JSON rank GET "${json}" CFBundleDocumentTypes ${i} LSHandlerRank)
    string(JSON role GET "${json}" CFBundleDocumentTypes ${i} CFBundleTypeRole)
    string(JSON icon GET "${json}" CFBundleDocumentTypes ${i} CFBundleTypeIconSystemGenerated)
    if(NOT rank STREQUAL "Owner" OR NOT role STREQUAL "Editor" OR NOT icon EQUAL 1)
      message(FATAL_ERROR "${job}: rank ${rank}, role ${role}, system icon ${icon}")
    endif()
    set(owned TRUE)
  endif()
endforeach()
if(NOT owned)
  message(FATAL_ERROR "no document type of the app opens ${job}")
endif()
message(STATUS ".fruitjob: ${job}, owned by ${app}")
