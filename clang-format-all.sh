#!/bin/bash -e
git ls-files -- \*.cpp \*.h \*.hpp \*.inc | xargs clang-format -i
