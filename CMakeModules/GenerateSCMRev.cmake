# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2019 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

# generate git/build information
include(GetSCMRev)

function(get_timestamp _var)
    string(TIMESTAMP timestamp UTC)
    set(${_var} "${timestamp}" PARENT_SCOPE)
endfunction()

get_timestamp(BUILD_DATE)

if (DEFINED GIT_RELEASE)
    set(BUILD_VERSION "${GIT_TAG}")
    set(GIT_REFSPEC "${GIT_RELEASE}")
    set(IS_DEV_BUILD false)
else()
    string(SUBSTRING ${GIT_COMMIT} 0 10 BUILD_VERSION)
    set(BUILD_VERSION "${BUILD_VERSION}-${GIT_REFSPEC}")
    set(IS_DEV_BUILD true)
endif()

if (NIGHTLY_BUILD)
    set(IS_NIGHTLY_BUILD true)
else()
    set(IS_NIGHTLY_BUILD false)
endif()

set(GIT_DESC ${BUILD_VERSION})

# Eden Duo: this script only runs when CMake (re)configures, so an incremental build
# (edit a .cpp, rebuild) never notices that HEAD moved and keeps stamping old builds
# with the GIT_COMMIT/GIT_REFSPEC captured at the last configure. Make the git ref
# files CMake watches for a re-run, so a build picks up a new commit without anyone
# re-running cmake by hand. ".git" is a file pointing elsewhere inside a worktree
# checkout (this repo's layout), so resolve the real per-worktree git dir and the
# shared common dir rather than assuming "${CMAKE_SOURCE_DIR}/.git" is a directory.
if (GIT_FOUND)
    run_git_command(EDEN_DUO_SCM_GIT_DIR rev-parse --git-dir)
    run_git_command(EDEN_DUO_SCM_GIT_COMMON_DIR rev-parse --git-common-dir)

    if (NOT EDEN_DUO_SCM_GIT_DIR MATCHES "NOTFOUND" AND NOT EDEN_DUO_SCM_GIT_COMMON_DIR MATCHES "NOTFOUND")
        if (NOT IS_ABSOLUTE "${EDEN_DUO_SCM_GIT_DIR}")
            set(EDEN_DUO_SCM_GIT_DIR "${CMAKE_SOURCE_DIR}/${EDEN_DUO_SCM_GIT_DIR}")
        endif()
        if (NOT IS_ABSOLUTE "${EDEN_DUO_SCM_GIT_COMMON_DIR}")
            set(EDEN_DUO_SCM_GIT_COMMON_DIR "${CMAKE_SOURCE_DIR}/${EDEN_DUO_SCM_GIT_COMMON_DIR}")
        endif()

        # Per-worktree HEAD: a plain commit hash while detached, "ref: refs/heads/x"
        # on a branch. Either way, moving HEAD (commit, checkout, switch) rewrites it.
        set(EDEN_DUO_SCM_HEAD_FILE "${EDEN_DUO_SCM_GIT_DIR}/HEAD")
        if (EXISTS "${EDEN_DUO_SCM_HEAD_FILE}")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${EDEN_DUO_SCM_HEAD_FILE}")

            file(READ "${EDEN_DUO_SCM_HEAD_FILE}" EDEN_DUO_SCM_HEAD_CONTENTS)
            if (EDEN_DUO_SCM_HEAD_CONTENTS MATCHES "^ref: (.*)$")
                string(STRIP "${CMAKE_MATCH_1}" EDEN_DUO_SCM_HEAD_REF)
                set(EDEN_DUO_SCM_REF_FILE "${EDEN_DUO_SCM_GIT_COMMON_DIR}/${EDEN_DUO_SCM_HEAD_REF}")
                if (EXISTS "${EDEN_DUO_SCM_REF_FILE}")
                    # Loose ref (the common case: a new commit rewrites this file).
                    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${EDEN_DUO_SCM_REF_FILE}")
                endif()
            endif()
        endif()

        # Fallback for a packed (e.g. post-gc) ref: watch packed-refs too.
        set(EDEN_DUO_SCM_PACKED_REFS "${EDEN_DUO_SCM_GIT_COMMON_DIR}/packed-refs")
        if (EXISTS "${EDEN_DUO_SCM_PACKED_REFS}")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${EDEN_DUO_SCM_PACKED_REFS}")
        endif()
    endif()
endif()

# Generate cpp with Git revision from template

# TODO(crueter): Stable releases feed.
set(BUILD_AUTO_UPDATE_STABLE_REPO "eden-emu/eden")
set(BUILD_AUTO_UPDATE_STABLE_API "git.eden-emu.dev")
set(BUILD_AUTO_UPDATE_STABLE_API_PATH "/api/v1/repos/")

set(BUILD_AUTO_UPDATE_API_PATH "/latest/release.json")
if (NIGHTLY_BUILD)
    set(BUILD_AUTO_UPDATE_WEBSITE "https://git.eden-emu.dev")
    set(BUILD_AUTO_UPDATE_API "nightly.eden-emu.dev")
    set(BUILD_AUTO_UPDATE_REPO "eden-ci/nightly")
    set(REPO_NAME "Eglantine Nightly")
else()
    set(BUILD_AUTO_UPDATE_WEBSITE "https://git.eden-emu.dev")
    set(BUILD_AUTO_UPDATE_API "stable.eden-emu.dev")
    set(BUILD_AUTO_UPDATE_REPO "eden-emu/eden")
    set(REPO_NAME "Eglantine")
endif()

set(BUILD_ID ${GIT_REFSPEC})
set(BUILD_FULLNAME "${REPO_NAME} ${BUILD_VERSION} ")
set(CXX_COMPILER "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

configure_file(scm_rev.cpp.in scm_rev.cpp @ONLY)
