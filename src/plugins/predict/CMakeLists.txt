# SPDX-License-Identifier: Apache-2.0 OR MIT
# hdrg.analyze.predict: the ANALYZE stage's learned check of the judged lights (a small MLP in
# integers, its weights from scripts/predictor/train.py)
# A plugin project (cmake/HdrgPlugin.cmake): built into the library from src/plugins, or apart
# against the installed package (a loadable module)
cmake_minimum_required(VERSION 3.29)
project(hdrgrading-predict LANGUAGES C)

if(NOT COMMAND hdrg_plugin)
    find_package(hdrgrading CONFIG REQUIRED)
endif()

hdrg_plugin(predict ENTRY hdrg_filter_predict_init
    SOURCES predict.c predict_weights.c
    LIBRARIES hdrgrading::foundation)
