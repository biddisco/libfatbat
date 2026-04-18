#!/usr/bin/env bash

# iterate over all files in the repo and reformat those that must be checked

for file in $(git ls-files | grep -E "\.(cpp|hpp|cu)(\.in)?$"); do
    # to allow for per-directory clang format files, we cd into the dir first
    DIR=$(dirname "$file")
    pushd ${DIR} >/dev/null
    clang-format-21 -i $(basename -- ${file})
    popd >/dev/null
done
