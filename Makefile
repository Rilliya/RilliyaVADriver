.PHONY: generate check build release

generate:
	./scripts/generate-project.sh

check:
	./scripts/check.sh

build:
	./scripts/build.sh Debug

release:
	./scripts/build.sh Release
