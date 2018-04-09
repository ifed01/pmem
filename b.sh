#!/bin/sh
c++ -g -std=c++17  main.cc persistent_objects.cc -I boost/include/ -lpthread

