# spdif-deframe: IEC 61937 deframing + in-process decode.
#
#   make            build the decoder
#   make check      regenerate the policy header and fail if it drifted, then run the tests
#   make policy     regenerate spdif-policy.h from policy.py (commit both)
#   make vectors    build the test-vector generator
#   make clean

CFLAGS  ?= -O2 -Wall -Wextra
PKG     := $(shell pkg-config --cflags --libs libavcodec libavutil)

.PHONY: all check policy vectors clean test
all: spdif-deframe

spdif-deframe: spdif-deframe.c spdif-policy.h
	$(CC) $(CFLAGS) -o $@ $< $(PKG) -lm

# The header is generated. A hand edit here fails the build rather than quietly diverging from
# policy.py, which is the single owner of the decision.
policy:
	./gen-policy.py

check: spdif-deframe vectors
	@./gen-policy.py --stdout > /tmp/spdif-policy.check.h
	@diff -u spdif-policy.h /tmp/spdif-policy.check.h \
	  || { echo "spdif-policy.h has DRIFTED from policy.py -- run 'make policy'"; exit 1; }
	@echo "policy header matches policy.py"
	@./tests/run-tests.sh

vectors: tests/make-vectors
tests/make-vectors: tests/make-vectors.c
	$(CC) $(CFLAGS) -o $@ $< $(PKG) -lm

test: check

clean:
	rm -f spdif-deframe tests/make-vectors tests/*.raw
