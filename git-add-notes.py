#!/usr/bin/env python3
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#

from typing import List
import subprocess
import sys

def from_pipe(argv: List[str]) -> str:
    """Executes argv as a command and returns its stdout."""
    result = subprocess.run(argv, capture_output=True)
    return result.stdout.strip().decode("utf-8", errors='replace')

def add_note(noteMsg: str, commit: str) -> None:
    """Add a note to a specific commit"""
    subprocess.run(["git", "notes", "add", "-m", noteMsg, commit], capture_output=True)
    print("Adding \"" + noteMsg + "\" to " + commit)

def get_change_id(git_cat_file: subprocess.Popen, hash_string: str) -> str:
    """Looks up the change-id for a git hash."""
    git_cat_file.stdin.write((hash_string + "\n").encode("utf-8"))
    git_cat_file.stdin.flush()
    first_line = git_cat_file.stdout.readline().decode("utf-8")
    size = first_line.strip().split(" ")[2]
    commit_msg = git_cat_file.stdout.read(int(size)).decode("utf-8")
    git_cat_file.stdout.readline()
    for line in commit_msg.split("\n"):
        if "Change-Id:" in line:
            return line
    return ""

def main() -> None:

    merge_base = from_pipe(["git", "merge-base", "collaboraoffice/online", "master"])
    to_change_ids = {}
    to_hash_string = from_pipe(["git", "rev-list", merge_base + "..master"])
    to_hashes = []
    if to_hash_string:
        to_hashes = to_hash_string.split("\n")
    git_cat_file = subprocess.Popen(['git', 'cat-file', '--batch'], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    for to_hash in to_hashes:
        to_change_ids[get_change_id(git_cat_file, to_hash)] = to_hash

    branch_head = from_pipe(["git", "rev-parse", "collaboraoffice/online"])
    commits_string = from_pipe(["git", "log", "--format=%H", "e89689b011ac8ce0d3af910e7c0a5eeaa3bfe3a8.." + branch_head])
    with open("not-cherry-picked-patches.txt", "w") as file:
        for commit in commits_string.split("\n"):
            commitNote = subprocess.run(["git", "notes", "show", commit], capture_output=True)

            # ignore commits with a note
            if commitNote.returncode == 0:
                continue

            engineLog = from_pipe(["git", "show", commit, "--", "engine/"])
            if not engineLog:
                # Add ignore note to commits outsite engine
                add_note("Auto: changes are not in engine/. No need to cherry-pick it", commit)
            else:
                changeid = get_change_id(git_cat_file, commit)
                if changeid in to_change_ids:
                    # Add not if the commit has been already cherry-picked. The changeId matches
                    add_note("Auto: cherry-picked in " + to_change_ids[changeid], commit)
                else:
                    pretty = from_pipe(["git", "--no-pager", "log", "-1", "--format=format:%h%x09%an%x09%ad%x09%s%x0a", "--date=short", commit]) + '\n'
                    file.write(pretty)

    git_cat_file.stdin.close()
    git_cat_file.terminate()

if __name__ == '__main__':
    main()
# vim:set shiftwidth=4 softtabstop=4 expandtab:
