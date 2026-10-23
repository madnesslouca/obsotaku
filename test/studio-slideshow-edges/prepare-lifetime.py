#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Copy exact plugin sources to a fresh directory with fixture-only checkpoints."""
import argparse
from pathlib import Path
import shutil
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('source',type=Path);p.add_argument('destination',type=Path)
p.add_argument('--variant',choices=['before','after'],required=True)
a=p.parse_args();assert not a.destination.exists(),'Refusing to replace existing evidence'
s=(a.source/'obs-slideshow-mk2.c').read_text()
assert s.count('#include <inttypes.h>')==1
s=s.replace('#include <inttypes.h>','#include <inttypes.h>\nextern bool studio_snapshot_checkpoint(obs_source_t *);\nextern float studio_snapshot_transition_time(obs_source_t *);\nextern void studio_snapshot_writer_checkpoint(void);',1)
if a.variant=='before':
 needle='return obs_source_get_ref(ss->transition);'
 replacement='obs_source_t *raw = ss->transition;\n\tif (!studio_snapshot_checkpoint(raw)) return NULL; /* avoid a stale dereference in the fixture */\n\treturn obs_source_get_ref(raw);'
else:
 needle='transition = obs_source_get_ref(ss->transition);'
 replacement='obs_source_t *raw = ss->transition;\n\tbool allowed = studio_snapshot_checkpoint(raw);\n\ttransition = allowed ? obs_source_get_ref(raw) : NULL;'
assert s.count(needle)==1;s=s.replace(needle,replacement,1)
start=s.index('static void ss_update(');needle='\tif (new_tr) {\n';index=s.index(needle,start)
s=s[:index]+s[index:].replace(needle,needle+'\t\tstudio_snapshot_writer_checkpoint();\n',1)
assert s.count('obs_transition_get_time(')==2
s=s.replace('obs_transition_get_time(','studio_snapshot_transition_time(')
s+='\n/* Fixture-only bridge to the actual static helper. */\nobs_source_t *studio_snapshot_get(obs_source_t *source) { return get_transition(obs_obj_get_data(source)); }\n'
a.destination.mkdir(parents=True)
shutil.copy2(a.source/'obs-slideshow.c',a.destination/'obs-slideshow.c')
(a.destination/'obs-slideshow-mk2.c').write_text(s)
