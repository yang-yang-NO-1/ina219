CXX ?= g++
CPPFLAGS += -Iinclude
CXXFLAGS ?= -O2 -std=c++11 -Wall -Wextra -Wpedantic
.PHONY: all test clean
all: ina219_monitor
ina219_monitor: src/main_orangepi.cpp include/ina219_api.h include/ina219_math.h include/battery_soc.h include/battery_soc_storage.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ src/main_orangepi.cpp
tests/ina219_api_test: tests/ina219_api_test.cpp include/ina219_api.h include/ina219_math.h include/battery_soc.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ tests/ina219_api_test.cpp
tests/battery_soc_test: tests/battery_soc_test.cpp include/battery_soc.h include/battery_soc_storage.h include/ina219_api.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ tests/battery_soc_test.cpp
test: tests/ina219_api_test tests/battery_soc_test
	./tests/ina219_api_test
	./tests/battery_soc_test
clean:
	rm -f ina219_monitor tests/ina219_api_test tests/battery_soc_test
