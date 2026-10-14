from pathlib import Path
import os,re,json
workspace=Path(os.environ.get('X620_WORKSPACE',Path(__file__).resolve().parents[3]))
repo=workspace/'linux-6.12'
os.environ.update(srctree=str(repo),ARCH='arm64',SRCARCH='arm64',CC='aarch64-linux-gnu-gcc',LD='aarch64-linux-gnu-ld',HOSTCC='gcc',KERNELVERSION='6.12.112')
reference=workspace/'artifacts/debian-kernel-config/debian-arm64.config'
seed=workspace/'cfg/x620.config'
core_prefix=('init/','kernel/','mm/','fs/','net/','security/','crypto/','lib/','arch/arm64/')
core_exact={'arch/Kconfig','drivers/base/Kconfig','drivers/connector/Kconfig','drivers/char/Kconfig','drivers/block/Kconfig','drivers/block/zram/Kconfig','drivers/md/Kconfig','drivers/dma-buf/Kconfig','drivers/input/Kconfig','drivers/tty/Kconfig','drivers/net/Kconfig'}
# Native olddefconfig remains authoritative for types, choices and dependencies.
# Here source declarations identify software scopes and symbols with user prompts.
nodes={};prompted=set()
for path in repo.rglob('Kconfig*'):
 if not path.is_file():continue
 rel=str(path.relative_to(repo))
 content=path.read_text(errors='replace')
 pieces=re.split(r'^\s*(?:menuconfig|config) (\w+)\s*$',content,flags=re.M)
 for i in range(1,len(pieces),2):
  name=pieces[i];body=pieces[i+1]
  nodes.setdefault(name,[]).append(rel)
  if re.search(r'^\s*(?:bool|tristate|int|hex|string|prompt)\s+"',body,re.M):prompted.add(name)
def classify(name):
 return any((file in core_exact or file.startswith(core_prefix)) and not file.endswith('Kconfig.platforms') for file in nodes.get(name,[]))
def parse(path):
 items={}
 for line in path.read_text().splitlines():
  m=re.match(r'CONFIG_(\w+)=(.*)',line)
  if m:items[m[1]]=m[2];continue
  m=re.match(r'# CONFIG_(\w+) is not set',line)
  if m:items[m[1]]='n'
 return items
ref=parse(reference);old=parse(seed);imported={}
for name,value in ref.items():
 if classify(name) and name in prompted:
  if any(tag in name for tag in ('AMD','INTEL','NVIDIA')):continue
  imported[name]=value
hardware={name:value for name,value in old.items() if not classify(name)}
# Keep the validated firmware handoff and storage/display bootstrap choices.
override={
 'LOCALVERSION':'"-x620-bringup"','LOCALVERSION_AUTO':'n','NR_CPUS':'10',
 'ARCH_MEDIATEK':'y','ARM64_4K_PAGES':'y','ARM64_16K_PAGES':'n','ARM64_64K_PAGES':'n',
 'ARM64_VA_BITS_48':'y','ARM64_VA_BITS_52':'n','COMPAT':'y',
 'PCI':'n','ACPI':'n','EFI':'n','CPU_IDLE':'n','CPU_FREQ':'n','SUSPEND':'n','HIBERNATION':'n',
 'RANDOMIZE_BASE':'n','X620_RELAY_DIAGNOSTICS':'y',
 'F2FS_FS':'y','EXT4_FS':'y','FAT_FS':'y','VFAT_FS':'y',
 'BLK_DEV_LOOP':'y','BLK_DEV_INITRD':'y','RD_GZIP':'y',
 'CMA':'y','DMA_CMA':'y','CMA_SIZE_MBYTES':'64',
 'MODULES':'y','MODULE_UNLOAD':'y','DEVTMPFS':'y','DEVTMPFS_MOUNT':'y',
 'ZRAM':'y','ZRAM_BACKEND_LZ4':'y','ZRAM_DEF_COMP_LZ4':'y',
 'PRINTK_TIME':'y','LOG_BUF_SHIFT':'20','IKCONFIG':'y','IKCONFIG_PROC':'y',
 'FONT_TER16x32':'y','FRAMEBUFFER_CONSOLE_DEFERRED_TAKEOVER':'n',
 'SYSTEM_TRUSTED_KEYS':'""','SYSTEM_REVOCATION_KEYS':'""',
}
profile={**imported,**hardware,**override}
lines=['# X620 distribution profile: Debian official arm64 6.12.107-1 software defaults.',
       '# Only hardware selection and validated firmware/boot constraints are overridden.',
       '# Baseline provenance and import details are generated in artifacts/distro-config-audit.json.']
for name,value in sorted(profile.items()):
 lines.append(f'# CONFIG_{name} is not set' if value=='n' else f'CONFIG_{name}={value}')
text='\n'.join(lines)+'\n'
seed.write_text(text);(repo/'arch/arm64/configs/x620_relay.config').write_text(text)
audit={'reference':'Debian official linux-config-6.12 6.12.107-1 arm64 (SHA256 recorded in artifacts/debian-kernel-config/source.json)',
 'software_source_scopes':list(core_prefix)+sorted(core_exact),'software_symbol_count':len(imported),'hardware_symbol_count':len(hardware),
 'board_overrides':override,'imported_software':imported,
 'purpose':'Preserve distro ABI, services, filesystems, networking, security and modules; retain X620 hardware/firmware handoff constraints',
 'effective_configuration_validation':'pending native olddefconfig and build'}
(workspace/'artifacts/distro-config-audit.json').write_text(json.dumps(audit,indent=2)+'\n')
print(json.dumps({'software_symbols':len(imported),'hardware_symbols':len(hardware),'profile_lines':len(lines)},indent=2))
