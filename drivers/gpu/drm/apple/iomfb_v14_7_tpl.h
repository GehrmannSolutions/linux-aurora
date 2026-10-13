/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Copyright The Asahi Linux Contributors */

/*
 * The classic IOMFB (dcpep) protocol of 14.7 firmware, from the shared
 * IOMFB template, for display coprocessors that Linux starts itself (the
 * T8122 dcpext). Not to be confused with iomfb_v14_7.h, Aurora's DCPLink
 * session for 14.7 processors handed over by the boot loader.
 */

#ifndef __APPLE_IOMFB_V14_7_TPL_H__
#define __APPLE_IOMFB_V14_7_TPL_H__

#include "version_utils.h"

#define DCP_FW v14_7_0
#define DCP_FW_VER DCP_FW_VERSION(14, 7, 0)

#include "iomfb_template.h"

#undef DCP_FW_VER
#undef DCP_FW

#endif /* __APPLE_IOMFB_V14_7_TPL_H__ */
