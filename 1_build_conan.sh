#!/bin/bash -e

use_deployer=0
for arg in "$@"; do
	if [[ "$arg" == "--use-deployer" ]]; then
		use_deployer=1
	fi
done

if [[ $(uname -s) == "Darwin" ]]; then
	cppstd=gnu23
else
	cppstd=23
fi

deployer_flags=()
if [[ $use_deployer -eq 1 ]]; then
	deployer_flags=(--deployer=full_deploy --deployer-folder=_d)
fi

for build_type in Debug Release; do
	conan install conanfile.txt \
		-b missing \
		-pr:b default \
		-of _d/cmake \
		"${deployer_flags[@]}" \
		-s build_type=${build_type} \
		-s compiler.cppstd=${cppstd}
done
