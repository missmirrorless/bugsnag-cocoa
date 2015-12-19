BUILD_FLAGS=-project Bugsnag.xcodeproj -scheme Bugsnag -sdk iphonesimulator9.2 -destination platform='iOS Simulator',name='iPhone 5' -configuration Debug
XCODEBUILD=set -o pipefail && xcodebuild
ifneq ($(strip $(shell which xcpretty)),)
 FORMATTER = | tee xcodebuild.log | xcpretty
endif

.PHONY: all build test

all: build

bootstrap:
	gem install xcpretty --quiet --no-ri --no-rdoc

build:
	$(XCODEBUILD) $(BUILD_FLAGS) build $(FORMATTER)

clean:
	$(XCODEBUILD) $(BUILD_FLAGS) clean $(FORMATTER)

test:
	$(XCODEBUILD) $(BUILD_FLAGS) test $(FORMATTER)

