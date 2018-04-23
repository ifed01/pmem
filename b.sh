#!/bin/sh
c++ -O3 -std=c++17  main.cc persistent_objects.cc allocator_test.cc -I boost/include/ -lpthread

