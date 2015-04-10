# Needs perftools support.
#
# @TEST-REQUIRES: bro  --help 2>&1 | grep -q mem-leaks
#
# @TEST-GROUP: leaks
#
# @TEST-EXEC: HEAP_CHECK_DUMP_DIRECTORY=. HEAPCHECK=local btest-bg-run bro bro -m -r $TRACES/http/bro.org.pcap $DIST/testing/btest/scripts/base/files/string_extract/test.bro
# @TEST-EXEC: btest-bg-wait 60
