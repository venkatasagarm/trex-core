# SPDX-License-Identifier: (BSD-3-Clause OR GPL-2.0)
# Copyright(c) 2019-2020 Pensando

allow_experimental_apis = true

deps += ['hash']
deps += ['common_ionic']

allow_experimental_apis = true

sources = files(
	'ionic_mac_api.c',
	'ionic_rx_filter.c',
	'ionic_rxtx.c',
	'ionic_rxtx_simple.c',
	'ionic_rxtx_sg.c',
	'ionic_dev.c',
	'ionic_dev_pci.c',
	'ionic_dev_vdev.c',
	'ionic_ethdev.c',
	'ionic_lif.c',
	'ionic_main.c'
)

includes += include_directories('../../common/ionic')

# TODO cflags += ['-DIONIC_EMBEDDED']
# TODO cflags += ['-DIONIC_MEM_BYPASS']
# TODO sources += files('ionic_mem_bypass.c')
# TODO cflags += ['-DDPDK_SIM']
