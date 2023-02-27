# SPDX-License-Identifier: BSD-4.
# Copyright(c) 2023 SolidRun ltd. All rights reserved.
# Author: Alvaro Karsz (alvaro.karsz@solid-run.com)
#
all: build

COMPILER ?= gcc
PROGRAM_NAME := tlv_parser

build:
	$(COMPILER) *.c -o $(PROGRAM_NAME)

install:
	@[ "${INSTALL_DIR}" ] || ( echo "Please set the INSTALL_DIR variable"; exit 1 )
	@[ -d "${INSTALL_DIR}" ] || ( echo "${INSTALL_DIR} is not a valid path"; exit 1 )
	@[ -f ${PROGRAM_NAME} ] || ( echo "Please make the program first"; exit 1 )
	cp -f $(PROGRAM_NAME) $(INSTALL_DIR)

clean:
	rm -f $(PROGRAM_NAME)
