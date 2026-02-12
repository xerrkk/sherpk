#!/bin/sh
cc -o /sbin/sherpk sherpk.c $(pkg-config --cflags --libs guile-3.0)
cc -static -o /sbin/erkl erkl.c
