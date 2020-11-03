#!/usr/bin/env python
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Copyright (C) 2019 Adek Maulana

'''
Version: 1.1
'''

from __future__ import print_function

import os
import sys
from argparse import ArgumentParser
from os import listdir
from os.path import isdir, exists, join
from subprocess import PIPE, Popen, CalledProcessError


def subprocess_run(cmd):
    subproc = Popen(cmd, stdout=PIPE, stderr=PIPE,
                    shell=True, universal_newlines=True)
    talk = subproc.communicate()
    exitCode = subproc.returncode
    if exitCode != 0:
        print('An error was detected while running the subprocess:\n'
              'exit code: %d\n'
              'stdout: %s\n'
              'stderr: %s' % (exitCode, talk[0], talk[1]))
        if exists('/tmp/merge-message'):
            os.remove('/tmp/merge-message')
        if 'CONFLICT' in talk[0]:
            print('Merge needs manual intervention!.')
            print('Resolve conflict(s) and `git commit` if you are done.')
            sys.exit(exitCode)
        else:
            raise CalledProcessError(exitCode, cmd)
    return talk


def git_env():
    cmd = 'git --version | cut -d " " -f3 | head -n1 | tr -d "\n"'
    talk = subprocess_run(cmd)
    version = talk[0].strip().split('.')
    git_version = (int(version[0]), int(version[1]))
    if git_version >= (2, 9):
        extra_cmd = '--allow-unrelated-histories'
    else:
        extra_cmd = ''
    return extra_cmd


def parameters():
    global wlan_type, merge_type, tag
    param = ArgumentParser(description='WLAN-CAF driver updater/initial '
                                       'merge into android kernel source.', )
    param.add_argument('-W', '--wlan', choices=['qcacld', 'prima'],
                       help='Your wlan driver type, either qcacld or prima.',
                       required=True)
    param.add_argument('-I', '--init', choices=['update', 'initial'],
                       help='Choose wether to update or initial merge.',
                       required=True)
    param.add_argument('-T', '--tag', help='Your current/target CAF TAG.',
                       required=True)
    params = vars(param.parse_args())
    wlan_type = params['wlan']
    merge_type = params['init']
    tag = params['tag']


def repo():
    global repo_url, staging, subdirs
    staging = 'drivers/staging'
    if wlan_type == 'qcacld':
        repo_url = {
            'fw-api': ('https://source.codeaurora.org/'
                       'quic/la/platform/vendor/qcom-opensource/wlan/fw-api'),
            'qca-wifi-host-cmn': ('https://source.codeaurora.org/'
                                  'quic/la/platform/vendor/qcom-opensource/'
                                  'wlan/qca-wifi-host-cmn'),
            'qcacld-3.0': ('https://source.codeaurora.org/'
                           'quic/la/platform/vendor/qcom-opensource/wlan/'
                           'qcacld-3.0')
        }
        subdirs = ['fw-api', 'qca-wifi-host-cmn', 'qcacld-3.0']
    elif wlan_type == 'prima':
        repo_url = {
            'prima': ('https://source.codeaurora.org/'
                      'quic/la/platform/vendor/qcom-opensource/wlan/prima')
        }
        subdirs = ['prima']


def check():
    if wlan_type == 'qcacld' and merge_type == 'initial':
        for subdir in subdirs:
            if isdir(join(staging, subdir)):
                if listdir(join(staging, subdir)):
                    print('%s exist and not empty' % subdir)
                    continue
                else:
                    if subdir == 'qcacld-3.0' and not listdir(
                        join(staging, 'fw-api')
                             ) and not listdir(
                        join(staging, 'qca-wifi-host-cmn')
                             ) and not listdir(join(staging, 'qcacld-3.0')):
                        return True
            else:
                return True
        else:
            print('\n' + 'You might want to use --init initial, '
                  'because those three are exists, '
                  '\nor one of them is exist and not empty.')
            raise OSError
    elif wlan_type == 'qcacld' and merge_type == 'update':
        for subdir in subdirs:
            if isdir(join(staging, subdir)):
                if not listdir(join(staging, subdir)):
                    print('%s exist and empty' % subdir)
                    continue
                else:
                    if subdir == 'qcacld-3.0' and listdir(
                        join(staging, 'fw-api')
                             ) and listdir(
                        join(staging, 'qca-wifi-host-cmn')
                             ) and listdir(join(staging, 'qcacld-3.0')):
                        return True
            else:
                continue
        else:
            print('\n' + 'You might want to use --init update, '
                  "because those three aren't exists."
                  '\nor exists but one of them has an empty folder.' + '\n')
            raise OSError
    elif wlan_type == 'prima' and merge_type == 'initial':
        if isdir(join(staging, subdirs)):
            if listdir(join(staging, subdirs)):
                print('\n' + 'You might want to use --init update, '
                      "\nbecause prima is exist and it's not empty." + '\n')
                raise OSError
            else:
                return True
        else:
            return True
    elif wlan_type == 'prima' and merge_type == 'update':
        if isdir(join(staging, subdirs)):
            if listdir(join(staging, subdirs)):
                return True
            else:
                print("Folder prima exist, but it's just an empty folder.")
                raise OSError
        else:
            print('You might want to use --init initial, '
                  "because prima isn't exist.")
            raise OSError


def merge():
    extra_cmd = git_env()
    merge_message = '/tmp/merge-message'
    if merge_type == 'initial':
        for repos in repo_url:
            print("Fetching '%s' with tag '%s'" % (repos, tag))
            cmd = 'git fetch --tags -f %s %s' % (repo_url[repos], tag)
            subprocess_run(cmd)
            merge_message = create_merge_message()
            with open(merge_message, 'r') as commit_file:
                commit = commit_file.read()
                with open(merge_message, 'w') as commit_file:
                    commit_file.write(repos + ': ' + commit)
            while True:
                cmds = [
                    'git merge -s ours --no-commit %s FETCH_HEAD' % extra_cmd,
                    ('git read-tree --prefix=drivers/staging/%s '
                     '-u FETCH_HEAD' % repos),
                    ('git commit --file %s --no-edit --quiet '
                     '--gpg-sign --signoff' % merge_message)
                ]
                for cmd in cmds:
                    subprocess_run(cmd)
                    if cmd == cmds[0]:
                        print("Merging '%s' into kernel source..." % repos)
                    if cmd == cmds[1]:
                        REPO = ['qcacld-3.0', 'prima']
                        if repos in REPO:
                            include_to_kconfig()
                    if cmd == cmds[2]:
                        print('Committing changes...')
                        if wlan_type != 'prima':
                            if (sys.version_info[0] < 3 and
                                    repos != 'qca-wifi-host-cmn'):
                                print()
                            elif (sys.version_info[0] >= 3 and
                                    repos != 'qcacld-3.0'):
                                print()
                if exists(merge_message):
                    os.remove(merge_message)
                break
    elif merge_type == 'update':
        for repos in repo_url:
            print("Fetching '%s' with tag '%s'" % (repos, tag))
            cmd = 'git fetch --tags -f %s %s' % (repo_url[repos], tag)
            subprocess_run(cmd)
            merge_message = create_merge_message()
            with open(merge_message, 'r') as commit_file:
                commit = commit_file.read()
                with open(merge_message, 'w') as commit_file:
                    commit_file.write(repos + ': ' + commit)
            while True:
                print("Merging '%s' into kernel source..."
                      % repos)
                cmds = [
                    ('git merge -X subtree=drivers/staging/%s FETCH_HEAD '
                     '--no-edit' % repos),
                    ('git commit --amend --file %s --no-edit --quiet '
                     '--gpg-sign --signoff' % merge_message)
                ]
                for cmd in cmds:
                    talk = subprocess_run(cmd)
                    if 'Already up to date.' in talk[0]:
                        print('Already up to date.')
                        break
                    else:
                        if cmd == cmds[1]:
                            print('Committing changes...')
                if wlan_type == 'qcacld':
                    if wlan_type != 'prima':
                        if (sys.version_info[0] < 3 and
                                repos != 'qca-wifi-host-cmn'):
                            print()
                        elif (sys.version_info[0] >= 3 and
                                repos != 'qcacld-3.0'):
                            print()
                if exists(merge_message):
                    os.remove(merge_message)
                break
    return


def include_to_kconfig():
    if merge_type == 'initial':
        tempRemove = 'endif # STAGING\n'
        KconfigToInclude = None
        if wlan_type == 'qcacld':
            KconfigToInclude = ('source "drivers/staging/qcacld-3.0/Kconfig"'
                                '\n\nendif # STAGING\n')
            KconfigToCheck = 'source "drivers/staging/qcacld-3.0/Kconfig"'
        elif wlan_type == 'prima':
            KconfigToInclude = ('source "drivers/staging/prima/Kconfig"'
                                '\n\nendif # STAGING\n')
            KconfigToCheck = 'source "drivers/staging/prima/Kconfig"'
        with open(join(staging, 'Kconfig'), 'r') as Kconfig:
            ValueKconfig = Kconfig.read()
        if KconfigToCheck not in ValueKconfig:
            if wlan_type == 'prima':
                print("Including 'prima' into kernel source...")
            elif wlan_type == 'qcacld':
                print("Including 'qcacld-3.0' into kernel source...")
            with open(join(staging, 'Kconfig'), 'w') as Kconfig:
                NewKconfig = ValueKconfig.replace(tempRemove, KconfigToInclude)
                Kconfig.write(NewKconfig)
            include_to_makefile()
            cmds = ['git add drivers/staging/Kconfig',
                    'git add drivers/staging/Makefile']
            for cmd in cmds:
                subprocess_run(cmd)
    return


def include_to_makefile():
    if merge_type == 'initial':
        with open(join(staging, 'Makefile'), 'r') as Makefile:
            MakefileValue = Makefile.read()
        if wlan_type == 'qcacld':
            ValueToCheck = 'CONFIG_QCA_CLD_WLAN'
            ValueToInclude = 'obj-$(CONFIG_QCA_CLD_WLAN)\t+= qcacld-3.0/'
        elif wlan_type == 'prima':
            ValueToCheck = 'CONFIG_PRONTO_WLAN'
            ValueToInclude = 'obj-$(CONFIG_PRONTO_WLAN)\t+= prima/'
        if ValueToCheck not in MakefileValue:
            with open(join(staging, 'Makefile'), 'a') as Makefile:
                Makefile.write(ValueToInclude)
    return


def get_previous_tag():
    revision = tag.split('-')[0]
    if merge_type == 'initial':
        previous_tag = None
        return previous_tag
    if wlan_type == 'qcacld':
        path = 'drivers/staging/qcacld-3.0'
    elif wlan_type == 'prima':
        path = 'drivers/staging/prima'
    cmd = ("git log --pretty=format:'%s' %s | grep '%s' "
           "| head -n 1" % ('%s', path, revision))
    talk = subprocess_run(cmd)
    if talk[0] is None:
        previous_tag = None
    else:
        comm = talk[0].strip('\n').replace("'", '').split()
        val = [t for t in comm if revision in t]
        try:
            previous_tag = val[0]
        except IndexError:
            previous_tag = None
    return previous_tag


def create_merge_message():
    merge_message = '/tmp/merge-message'
    previous_tag = get_previous_tag()
    tags = 'None'
    cmds = [
            'git rev-parse --abbrev-ref HEAD',
            'git rev-list --count %s' % tags,
            ('git log --oneline --pretty=format:"        %s" "%s"'
             % ('%s', tags))
    ]
    if previous_tag is not None and merge_type == 'update':
        range = 'refs/tags/%s..refs/tags/%s' % (previous_tag, tag)
        for cmd, value in enumerate(cmds):
            cmds[cmd] = value.replace(tags, range)
    elif previous_tag is None and merge_type == 'initial':
        for cmd, value in enumerate(cmds):
            cmds[cmd] = value.replace(tags, tag)
        # don't add all commit changes in initial
        command = ('git log --oneline --pretty=oneline -45 --pretty=format'
                   ':"        %s" "%s"' % ('%s', tag))
        for cmd, value in enumerate(cmds):
            cmds[cmd] = value.replace(cmds[2], command)
    for cmd in cmds:
        talk = subprocess_run(cmd)
        if cmd == cmds[0]:
            branch = talk[0].strip('\n')
        if cmd == cmds[1]:
            total_changes = talk[0].strip('\n')
        if cmd == cmds[2]:
            commits = talk[0]
    with open(merge_message, 'w+') as commit_msg:
        if merge_type == 'initial':
            commit_msg.write("Initial tag '%s' into %s" % (tag, branch))
        else:
            commit_msg.write("Merge tag '%s' into %s" % (tag, branch))
        commit_msg.writelines('\n' + '\n')
        if merge_type == 'initial':
            commit_msg.write('This is an initial merged, '
                             "all commit changes will not be written fully.\n")
        commit_msg.write("Changes in tag '%s': (%s commits)"
                         % (tag, total_changes))
        commit_msg.writelines('\n')
        commit_msg.write(commits)
        if merge_type == 'initial':
            commit_msg.write('\n' + '        ...')
    return merge_message


def main():
    print()
    repo()
    if not exists('Makefile'):
        print('Run this script inside your root kernel source.')
        raise OSError
    if not exists(staging):
        print("Staging folder can't be found, "
              'are you sure running it inside kernel source?')
        raise OSError
    if check() is True:
        merge()
    if exists('/tmp/merge-message'):
        os.remove('/tmp/merge-message')


if __name__ == '__main__':
    parameters()
    main()
