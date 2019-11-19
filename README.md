# autotier
Moves files with an mtime older than the configured threshold to a lower tier of storage, et vice versa if the file is modified.

## What it does
If it finds a file in a defined storage tier that hasn't been written to in the specified expiry time, it moves the file to the next lowest storage tier and replaces the original with a symlink to the new location. Conversely, if this file is opened and modified (through the symlink or otherwise), the symlink is removed and the file is moved back to it's original tier. This behaviour cascades across as many storage tiers as you want to define in the configuration file.

## Usage
Create a [crontab](https://linux.die.net/man/5/crontab) entry to periodically run `autotier`. The default configuration file is `/etc/autotier.conf`, but this can be changed by passing the `-c`/`--config` flag followed by the path to the alternate configuration file. The first defined tier should be the working tier that is exported.

## Configuration
The layout of a single tier's configuration entry is as follows:
```
[<Tier name>]
DIR=/path/to/storage/tier
EXPIRES=<number of seconds until file expiry>
```
As many tiers as desired can be defined in the configuration, however they must be in order of fastest to slowest.  
Below is an example of a configuration file:
```
# autotier.conf
[Fastest Tier]
DIR=/tier_1    # fast tier storage pool
EXPIRES=3600   # one hour

[Medium Tier]
DIR=/tier_2
EXPIRES=7200   # two hours

[Slowest Tier]
DIR=/tier_3
EXPIRES=14400  # four hours
```
