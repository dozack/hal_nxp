# Copyright 2024 NXP
#
# SPDX-License-Identifier: BSD-3-Clause

if(CONFIG_MCUX_COMPONENT_driver.ctimer)
    mcux_component_version(2.3.1)

    mcux_add_source(SOURCES fsl_ctimer.h fsl_ctimer.c)

    mcux_add_include(INCLUDES .)

endif()
