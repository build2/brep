#! /bin/sh

# Normally, you don't need to regenerate the private key.
#
# openssl genrsa 4096 > key.pem

# Copy cert.pem content to the certificate value of the following manifest
# files:
#   1/stable/repositories
#   pkg/1/dev.cppget.org/signed/repositories
#
openssl req -x509 -new -key key.pem -days 365 -config openssl.cnf > cert.pem

# To regenerate the packages and signature manifest files run:
#
# ../../../bpkg/bpkg/bpkg rep-create 1/stable --key key.pem
# ../../../bpkg/bpkg/bpkg rep-create pkg/1/dev.cppget.org/signed --key key.pem
