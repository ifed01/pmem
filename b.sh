#!/bin/sh
c++ -g -std=c++17  main.cc persistent_objects.cc fastbmap_allocator_impl.cc -I boost/include/ -lpthread -DNON_CEPH_BUILD

