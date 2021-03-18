# autotier
A passthrough FUSE filesystem that intelligently moves files between storage tiers based on frequency of use, file age, and tier fullness.

## What it does
`autotier` is a tiered FUSE filesystem which acts as a merging passthrough to any number of underlying filesystems. These underlying filesystems can be of any type. Behind the scenes, `autotier` moves files around such that the most often accessed files are kept in the highest tier. `autotier` fills each defined tier up to their configuration-defined quota, starting at the fastest tier with the highest priority files. If you do a lot of writing, set a lower quota for the highest tier to allow for more room. If you do mostly reading, set a higher watermark to allow for as much use as possible out of your available top tier storage.  
![autotier example](doc/mounted_fs_status.png)

## Installation
### Current Release
* Get deb: ```$ wget https://github.com/45Drives/autotier/releases/download/v1.1.0/autotier_1.1.0-1focal_amd64.deb```
* Install deb: `# dpkg -i autotier_1.1.0-1focal_amd64.deb`
* Edit configuration file: `/etc/autotier.conf`
* Mount filesystem:
	* manually: `# autotierfs /path/to/mountpoint -o allow_other,default_permissions`
	* fstab: `/usr/bin/autotierfs	/path/to/mountpoint	fuse	allow_other,default_permissions 0 0`

### Installing from Source
* Install dependencies:  
	```# apt install libfuse3-dev libstdc++-dev libboost-system-dev libboost-filesystem-dev libboost-serialization-dev librocksdb-dev libtbb-dev```
* `$ git clone https://github.com/45drives/autotier`
* `$ cd autotier`
* `$ git checkout <version>` (v1.1.0 is the latest release)
* `$ make -j8` (or `make -j8 no-par-sort` to use c++11 instead of c++17)
* `# make install`
* Edit configuration file
* Mount filesystem  

### Uninstallation
From dpkg: `# dpkg --remove autotier`  
From source: `# make uninstall` from root of cloned repo

## Configuration
### Global Config
For global configuration of `autotier`, options are placed below the `[Global]` header. Example:
```
[Global]                       # global settings
Log Level = 1                  # 0 = none, 1 = normal, 2 = debug
Tier Period = 100              # number of seconds between file move batches
```
The global config section can be placed before, after, or between tier definitions.
### Tier Config
The layout of a single tier's configuration entry is as follows:
```
[Tier 1]                       # tier name (can be anything)
Path =                         # full path to tier storage pool
Quota =                        # absolute or % usage to keep tier under
# Quota format: x (%|B|MB|MiB|KB|KiB|MB|MiB|...)
# Example: Quota = 5.3 TiB
```
As many tiers as desired can be defined in the configuration, however they must be in order of fastest to slowest. The tier's name can be whatever you want but it cannot be `global` or `Global`. Tier names are only used for config diagnostics and file pinning.  
Below is a complete example of a configuration file:
```
# autotier config
[Global]                       # global settings
Log Level = 1                  # 0 = none, 1 = normal, 2 = debug
Tier Period = 1000             # number of seconds between file move batches

[Tier 1]                       # tier name (can be anything)
Path = /mnt/ssd_tier           # full path to tier storage pool
Quota = 5 TiB                  # absolute or % usage to keep tier under

[Tier 2]
Path = /mnt/ssd_tier
Quota = 90 %

[Tier 3]
Path = /mnt/cold_storage
Quota = 100 %
```

## Usage
See `man cephgeorep` after installing for full usage details.
### Command Line Tool Usage
```
Usage:
  mount filesystem:
    autotier [<flags>] <mountpoint> [-o <fuse,options,...>]
  ad hoc commands:
    autotier [<flags>] <command> [<arg1 arg2 ...>]
Commands:
  config      - display current configuration file
  help        - display this message
  list-pins   - show all pinned files
  list-popularity
              - print list of all tier files sorted by frequency of use
  oneshot     - execute tiering only once
  pin <"tier name"> <"path/to/file" "path/to/file" ...>
              - pin file(s) to tier using tier name in config file
  status      - list info about defined tiers
  unpin <"path/to/file" "path/to/file" ...>
              - remove pin from file(s)
  which-tier <"path/to/file" "path/to/file" ...>
              - list which tier each argument is in
Flags:
  -c, --config <path/to/config>
              - override configuration file path (default /etc/autotier.conf)
  -h, --help  - display this message and cancel current command
  -o, --fuse-options <comma,separated,list>
              - mount options to pass to fuse (see man mount.fuse)
  -q, --quiet - set log level to 0 (no output)
  -v, --verbose
              - set log level to 2 (debug output)
  -V, --version
              - print version and exit
              - if log level >= 1, logo will also print
              - combine with -q to mute logo output
```
Examples:  
Trigger tiering of files immediately:  
`autotier oneshot`  
Show status of configured tiers:  
`autotier status`  
Pin a file to a tier with \<Tier Name\>:  
`autotier pin "<Tier Name>" /path/to/file`  
Pin multiple files:  
`autotier pin "<Tier Name>" /path/to/file1 /path/to/dir/* /bash/expansion/**/*`  
`find path/* -type f -print | xargs autotier pin "<Tier Name>"`  
Remove pins:  
`autotier unpin path/to/file`  
`find path/* -type f -print | xargs autotier unpin`  
List pinned files:  
`autotier list-pins`

---
```
   ┓
└─ ┃ ├─
└─ ┣ ├─
└─ ┃ └─
   ┛
```
[![45Drives Logo](https://www.45drives.com/img/45-drives-brand.png)](https://www.45drives.com)
