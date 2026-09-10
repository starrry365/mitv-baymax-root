import sys, capstone
d=open(sys.argv[1],'rb').read()
vaddr=int(sys.argv[2],16); ln=int(sys.argv[3])
code=d[vaddr:vaddr+ln]
md=capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB|capstone.CS_MODE_LITTLE_ENDIAN)
for ins in md.disasm(code, vaddr):
    print('%06x  %-10s %s'%(ins.address, ins.mnemonic, ins.op_str))
