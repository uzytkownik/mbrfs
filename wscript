#! /usr/bin/env python
# encoding: utf-8

VERSION='0.0.0'

srcdir = '.'
blddir = 'build'

def set_options(opt):
    opt.tool_options('compiler_cc')

def configure(conf):
    conf.check_tool('compiler_cc')
    conf.check_cfg(package='fuse', uselib_store='FUSE',
                   atleast_version='2.6.0', mandatory=1,
                   args='--cflags --libs')
    conf.define('VERSION', VERSION)
    conf.write_config_header('config.h')

def build(bld):
    mbrfs = bld.new_task_gen('cc', 'program')
    mbrfs.find_sources_in_dirs('.')
    mbrfs.target='mbrfs'
    mbrfs.packages='fuse'
    mbrfs.uselib='FUSE'

