"""MK1 rev 5.0 main CPU (TMS34010) ahead-of-time translation from the user's program image.

One aligned scan of the authenticated image (tools/mk2/offline_decode.scan_aligned, the decoder compiled from the pinned
MAME sources by tools/setup/decoder.py) gives size and assembler text of every instruction. The generator emits:
  mk5_main_loop_graph.inc   fixed labels of the main-loop graph (PCs: tools/mk1/partitions.json "main_loop")
  mk5_task_body_graph.inc   fixed labels of the first task-body graph (PCs: partitions.json "task_body")
  mk5_boot_graph.inc        fixed labels of the power-on graph: reset, boot, interrupt handler and the CMOS/DSW
                            routines the task and main graphs call (PCs: partitions.json "boot")
Inputs are only addresses and the ROM; the emitter never reads a generated file. Output goes to generated/mk1/<sha>/
(tools/setup/mk1.py); no runtime opcode decoding exists in the product.
"""
from pathlib import Path
import hashlib,json,re,sys
sys.path.insert(0,str(Path(__file__).resolve().parents[2]))
from tools.mk1 import roms

ROOT=Path(__file__).resolve().parents[2]
PARTITIONS=ROOT/'tools/mk1/partitions.json'
PROGRAM=dict(sha256=roms.manifest()['program_sha256'],size=1<<20,base=roms.BASE)


def scan(rom):
    """The single decoder run of a generation: every aligned position of the authenticated image."""
    from tools.mk2.offline_decode import scan_aligned
    return scan_aligned(rom,PROGRAM)


def table(name):
    data=json.loads(PARTITIONS.read_text('utf-8'))
    if data.get('program_sha256')!=PROGRAM['sha256']:raise ValueError('partitions.json names another program image')
    return [int(x,16) for x in data[name].split()]


def graph_rows(rows,pcs):
    """{pc: (size, assembler text, first opcode word)} of the table PCs; data words and truncated fetches are refused."""
    out={}
    for pc in pcs:
        size,_,raw,asm=rows[pc]
        if not size or asm.startswith('DW'):raise ValueError(f'partitions.json names a non-instruction at {pc:08x}')
        out[pc]=(size,asm,raw[0])
    return out


def emit_graph(rows,M,boot=False):
    """Labels of one TMS34010 graph: rows {pc: (size bits, assembler text)}; M selects the main-loop forms (every
    main-graph extension) or the task-body forms. boot (with M) emits the power-on graph run by Mk5TaskBodyRunner::
    execute_boot(): a PC outside the graph returns to the runner's dispatcher instead of stopping, SETF costs 1+F
    (34010ops.hxx), PIXT and PIXBLT B,XY use the runner's TMS34010 graphics semantics, and RETS ends a subroutine
    run (native_block) when it pops the caller's frame. Returns (text, unsupported)."""
    def imm(a):
        assert re.fullmatch('[0-9A-F]+h',a),a
        return int(a[:-1],16)
    def reg(a):
        if a=='SP':return 's.a[15]'
        assert re.fullmatch('[AB](?:[0-9]|1[0-5])',a),a
        # Both banks' register15 are the physical shared stack pointer.
        return f's.{"a" if a[1:]=="15" else a[0].lower()}[{int(a[1:])}]'
    def value(a):return f'0x{imm(a):x}u' if a.endswith('h') else reg(a)
    def regs(a):
        out=[]
        for x in a.split(','):
            if '-' in x:
                l,r=x.split('-');out.extend(l[0]+str(i) for i in range(int(l[1:]),int(r[1:])+1))
            else:out.append(x)
        return out
    def address(a,width,pre,post):
        if a.startswith('@'):return f'0x{imm(a[1:]):x}u'
        if a.startswith('-*'):
            r=reg(a[2:]);pre.append(f'{r}-={width};');return r
        assert a.startswith('*'),a
        x=a[1:];offset=0
        if x.endswith('+'):
            x=x[:-1];post.append(f'{reg(x)}+={width};')
        if '(' in x:
            x,o=x[:-1].split('(');offset=imm(o)
            if offset&0x8000:offset-=65536
        return f'({reg(x)}+uint32_t({offset}))'
    def jump(t):return f'goto pc_{t:08x};' if t in rows else f's.next_original_pc=0x{t:08x}; goto continuation;'
    conditions={'JR':'true','JREQ':'(s.status&0x20000000)!=0','JRNE':'(s.status&0x20000000)==0','JRC':'(s.status&0x40000000)!=0','JRNC':'(s.status&0x40000000)==0','JRN':'(s.status&0x80000000)!=0','JRNN':'(s.status&0x80000000)==0','JRV':'(s.status&0x10000000)!=0','JRNV':'(s.status&0x10000000)==0','JRLT':'((s.status>>31)^(s.status>>28))&1','JRGE':'!(((s.status>>31)^(s.status>>28))&1)','JRGT':'!(s.status&0x20000000) && !(((s.status>>31)^(s.status>>28))&1)','JRLE':'(s.status&0x20000000) || (((s.status>>31)^(s.status>>28))&1)','JRHI':'!(s.status&0x60000000)','JRLS':'(s.status&0x60000000)!=0'}
    if M:conditions.update(JAEQ='(s.status&0x20000000)!=0',JRP='!(s.status&0xa0000000)',JAC='(s.status&0x40000000)!=0')
    if M:conditions.update(JANE='(s.status&0x20000000)==0',JANC='(s.status&0x40000000)==0')
    unsupported=[];out=['// Generated offline by tools/'+('mk1/generate.py (power-on graph)' if boot else 'generate_mk5_task_body.py')+'. Fixed native code.','continuation:','switch(s.next_original_pc){']
    if not M:out.append('case 0xffc955a0: pre(0xffc955a0); throw TaskBodyStop::MainLoop;')
    for pc in sorted(rows):out.append(f'case 0x{pc:08x}: goto pc_{pc:08x};')
    for pc in [0xffe2a930,0xffe13c10,0xffe1a310]:
        if pc not in rows and not boot:out.append(f'case 0x{pc:08x}: pre(0x{pc:08x}); native_block(0x{pc:08x}); goto continuation;')
    out.extend(['default: '+('return;' if boot else 'throw TaskBodyStop::Untranslated;'),'}'])
    for pc,(size,asm,word) in sorted(rows.items()):
        op,*tail=asm.split(None,1);arg=tail[0] if tail else '';a=arg.split(',');n=pc+size;code=[];cost=1;terminal=False
        try:
            if op=='MOVE' and len(a)==2 and any(x.startswith('@') for x in a):op='MOVB' # decoder alias 05e0/07e0
            if op in ['MOVE','MOVB']:
                if len(a)==2 and all(re.fullmatch(r'(?:[AB]\d+|SP)',x) for x in a):code=[f'load({reg(a[1])},{reg(a[0])});']
                else:
                    width='8' if op=='MOVB' else f'fw({a[2]})';before=[];after=[]
                    srcmem=a[0].startswith(('*','-*','@'));dstmem=a[1].startswith(('*','-*','@'))
                    src=address(a[0],width,before,after) if srcmem else reg(a[0]);code+=before;before=[]
                    if srcmem and not M and op=='MOVE' and a[2]=='1' and not dstmem:
                        # Task-body read() sign-extends by FE0 only; field 1 follows FE1 (34010ops.hxx MOVE ...,1, RFIELD1).
                        code.append(f'uint32_t v=read(0x{pc:08x},{src},{width},false); {{unsigned w={width}; if((s.status&0x800) && w<32 && (v&(1u<<(w-1))))v|=~((1u<<w)-1);}}');code+=after;after=[];src='v'
                    elif srcmem:code.append(f'uint32_t v=read(0x{pc:08x},{src},{width}'+(',true' if op=='MOVB' else f',(s.status&(0x20u<<({a[2]}*6)))!=0')*M+');');code+=after;after=[];src='v'
                    dst=address(a[1],width,before,after) if dstmem else reg(a[1]);code+=before
                    code.append(f'write(0x{pc:08x},{dst},{width},{src});' if dstmem else f'load({dst},{src});');code+=after
                    if srcmem and ('SP' in a[0] or 'B15' in a[0]):code.append('++f.check.stack_read_count;')
                    if not srcmem:cost=2 if a[1].startswith('-*') else (3 if '(' in a[1] or a[1].startswith('@') else 1)
                    elif not dstmem:cost=4 if a[0].startswith('-*') else (5 if '(' in a[0] or a[0].startswith('@') else 3)
                    else:cost=7 if a[0].startswith('@') else (5 if '(' in arg else (4 if '+' in arg or '-*' in arg else 3))
                    if op=='MOVB' and not srcmem and a[1].startswith('@'):cost=1
            elif op=='MOVI':
                v=imm(a[0]);v=v|0xffff0000 if size==32 and v&0x8000 else v;code=[f'load({reg(a[1])},0x{v:x}u);'];cost=size//16
            elif op=='MOVK':code=[f'{reg(a[1])}={value(a[0])};']
            elif op in ['ADD','ADDI','ADDK','SUB','SUBI','SUBK','CMP','CMPI','ADDC']:
                fn='compare' if op.startswith('CMP') else ('addc' if op=='ADDC' else ('sub' if op.startswith('SUB') else 'add'));v=value(a[0]);
                if size==32 and op in ['ADDI','SUBI','CMPI'] and imm(a[0])&0x8000:v=f'0x{imm(a[0])|0xffff0000:x}u'
                code=[f'{fn}({reg(a[1])},{v});'];cost=size//16
            elif op in ['INC','DEC']:code=[f'{"add" if op=="INC" else "sub"}({reg(a[0])},1);']
            elif op in ['CLR','NOT','NEG']:
                r=reg(a[0]);code=[f'{r}={"0" if op=="CLR" else "~"+r}; z({r});'] if op!='NEG' else [f'uint32_t v={r}; {r}=0; sub({r},v);']
            elif op=='ABS' and M:
                # TMS34010 NZV describe the negated input; C is unchanged. Unsigned
                # subtraction also defines the INT_MIN case without signed overflow.
                r=reg(a[0]);code=[f'uint32_t negative=0u-{r}; if(negative && !(negative&0x80000000u)){r}=negative; s.status=(s.status&~uint32_t(0xb0000000))|(negative&0x80000000u)|(negative?0u:0x20000000u)|(negative==0x80000000u?0x10000000u:0u);']
            elif op in ['AND','OR','XOR','ANDI','ORI','XORI','ANDN']:
                sign='&' if op.startswith('AND') else ('|' if op.startswith('OR') else '^');v=value(a[0]);v='~'+v if op=='ANDN' else v;code=[f'{reg(a[1])}{sign}={v}; z({reg(a[1])});'];cost=size//16
            elif op in ['SLL','SRL','SLA','RL']:
                # 34010ops.hxx SRL(R,K): count is (-K)&31; the K operand is already decoded, a register Rs is not.
                r=reg(a[1]);v=value(a[0]) if op!='SRL' or a[0].endswith('h') else f'0u-{reg(a[0])}';fn='sla' if op=='SLA' else ('rotate' if op=='RL' else 'shift');extra='' if op in ['SLA','RL'] else (',true' if op=='SLL' else ',false');code=[f'{fn}({r},{v}{extra});'];cost=3 if op=='SLA' else 1
            elif op=='SRA' and M:
                r=reg(a[1]);code=[f'unsigned k={value(a[0]) if a[0].endswith("h") else "(0u-"+reg(a[0])+")"}&31; uint32_t old={r}; s.status&=~uint32_t(0xe0000000); if(k){{s.status|=((old>>(k-1))&1)<<30; {r}=uint32_t(int32_t(old)>>k);}} s.status|={r}&0x80000000; z({r});']
            elif op=='LMO' and M:
                code=[f'uint32_t v={reg(a[0])},count=0; z(v); if(v)while(!(v&0x80000000)){{++count;v<<=1;}} {reg(a[1])}=count;']
            elif op=='DIVS' and M and (pc in (0xffdabf00,0xffdabf80) or (M and pc in (0xffd6e9c0,0xffdcb700,0xffdcb790)) or (M and pc in (0xffda0ed0,0xffda0ee0)) or (M and pc in (0xffd8e180,0xffd8e1f0,0xffd8e400,0xffd8e460))):
                # Fixed authenticated odd-destination labels only; no generic DIVS.
                # Coverage adds three explicit contracts; all other guards stay intact.
                assert arg=={0xffdabf00:'A1,A3',0xffdabf80:'A3,A1',0xffd6e9c0:'A1,A5',0xffdcb700:'A2,A7',0xffdcb790:'A2,A3',0xffda0ed0:'A0,A3',0xffda0ee0:'A0,A5',0xffd8e180:'A10,A3',0xffd8e1f0:'A1,A3',0xffd8e400:'A10,A3',0xffd8e460:'A1,A3'}[pc]
                r=reg(a[1]);src=reg(a[0])
                code=[f'int64_t dividend=int32_t({r}),divisor=int32_t({src}); if(dividend==-2147483648LL && divisor==-1)throw TaskBodyStop::Untranslated; s.status&=~uint32_t(0xb0000000); if(!divisor){{s.status|=0x10000000;}}else{{{r}=uint32_t(dividend/divisor);s.status|={r}&0x80000000;z({r});}}'];cost=39
            elif op=='DIVU' and M:
                r=reg(a[1]);i=int(a[1][1:]);other=reg(a[1][0]+str(i|1))
                code=[f'uint32_t divisor={reg(a[0])}; s.status&=~uint32_t(0x30000000); if(!divisor){{s.status|=0x10000000;}} else {{']
                if i%2==0:
                    code.append(f'uint64_t dividend=(uint64_t({r})<<32)|{other},q=dividend/divisor; if(q>0xffffffff){{s.status|=0x10000000;}}else{{{r}=uint32_t(q);{other}=uint32_t(dividend%divisor);z({r});}}')
                else:code.append(f'{r}/=divisor;z({r});')
                code.append('}');cost=37
            elif op=='MPYS' and M:
                r=reg(a[1]);i=int(a[1][1:]);code=[f'uint32_t src={reg(a[0])}; unsigned w=fw(1); if(w<32){{src&=(1u<<w)-1; if(src&(1u<<(w-1)))src|=~((1u<<w)-1);}} int64_t product=int64_t(int32_t(src))*int32_t({r}); s.status&=~uint32_t(0xa0000000); s.status|=uint32_t(uint64_t(product)>>32)&0x80000000; z(product?1:0); {r}=uint32_t(uint64_t(product)>>32); {reg(a[1][0]+str(i|1))}=uint32_t(product);'];cost=20
            elif op=='PIXT' and M and a[0].startswith('*') and not a[1].startswith('*'):
                if boot:code=[f'if(!(irq.io()[8]&0x800)){{throw TaskBodyStop::Untranslated;}} {reg(a[1])}=shift_load({reg(a[0][1:])}); s.status=(s.status&~uint32_t(0x10000000))|({reg(a[1])}?0x10000000:0);']
                else:code=[f'if(!video || !(irq.io()[8]&0x800)){{throw TaskBodyStop::Untranslated;}} {reg(a[1])}=video->ShiftLoad({reg(a[0][1:])}); s.status=(s.status&~uint32_t(0x10000000))|({reg(a[1])}?0x10000000:0);']
                cost=4
            elif op=='PIXBLT' and boot and arg=='B,XY':
                code=[f'uint32_t cycles=pixblt_b_xy(0x{pc:08x}); step(0x{pc:08x},0x{n:08x},cycles);',jump(n)];terminal=True
            elif op=='FILL' and M and arg=='XY':
                code=[f'if(!video){{throw TaskBodyStop::Untranslated;}} uint32_t cycles=video->FillXY(s); step(0x{pc:08x},0x{n:08x},cycles);',jump(n)];terminal=True
            elif op=='EINT' and M:
                code=[f'bool taken=irq.Eint(0x{pc:08x},0x{n:08x}); video_update(); result.instructions.push_back({{0x{pc:08x},0x{n:08x},3}}); if(taken || irq.CheckBoundary(0x{pc:08x}))throw TaskBodyStop::IrqHandler;',jump(n)];terminal=True
            elif op=='RETI' and M:
                code=[f'if(!active_handler){{throw TaskBodyStop::Untranslated;}} f.check.stack_read_count+=2; bool taken=irq.Reti(0x{pc:08x}); ++di.completed_handlers;active_handler=false; video_update(); result.instructions.push_back({{0x{pc:08x},s.next_original_pc,11}}); if(taken || irq.CheckBoundary(0x{pc:08x}))throw TaskBodyStop::IrqHandler; goto continuation;'];terminal=True
            elif op=='BTST':code=[f'z({reg(a[1])}&(1u<<({value(a[0])}&31)));'];cost=1 if a[0].endswith('h') else 2
            elif op in ['SETC','CLRC']:code=[f's.status{"|=" if op=="SETC" else "&=~"}uint32_t(0x40000000);']
            elif op in ['MOVX','MOVY']:
                src,dst=reg(a[0]),reg(a[1]);code=[f'{dst}=({dst}&0x{("ffff0000" if op=="MOVX" else "0000ffff")})|({src}&0x{("0000ffff" if op=="MOVX" else "ffff0000")});']
            elif op in ['ADDXY','SUBXY']:code=[f'xy({reg(a[1])},{reg(a[0])},{str(op=="SUBXY").lower()});']
            elif op=='CMPXY' and M:
                code=[f'uint16_t x=uint16_t({reg(a[1])}-{reg(a[0])}),y=uint16_t(({reg(a[1])}>>16)-({reg(a[0])}>>16)); s.status&=0x0fffffff; if(!x)s.status|=0x80000000; if(y&0x8000)s.status|=0x40000000; z(y); if(x&0x8000)s.status|=0x10000000;']
            elif op in ['ZEXT','SEXT']:
                r=reg(a[0]);width=f'fw({a[1]})';code=[f'unsigned w={width}; if(w<32){r}&=(1u<<w)-1;'+(f' if(w<32 && ({r}&(1u<<(w-1)))){r}|=~((1u<<w)-1);' if op=='SEXT' else '')+f' z({r});']
                # 34010ops.hxx SEXT: NZ from the result, 3 cycles in both graphs.
                if op=='SEXT':code[0]+=f' s.status=(s.status&~uint32_t(0x80000000))|({r}&0x80000000);';cost=3
            # Open (front D, 15/11): MAME 34010ops.hxx SETF encodes FS 32 as 0 and costs 1+F cycles. Emitting that clears
            # FE1 and arms the post-picture native_block shortcut (status 0x00200030), which stops every MK1 route at
            # ffe13c10 frame 135 (front-d-regress.json); kept as emitted until that host contract is decided.
            elif op=='SETF':
                code=[f's.status=(s.status&~uint32_t(0x3fu<<({a[2]}*6)))|((({imm(a[0])}|({a[1]}<<5))&0x3f)<<({a[2]}*6));']
                # Power-on graph: 34010ops.hxx SETF, FS is five bits (32 encodes as 0), cost 1+F.
                if boot:code=[f's.status=(s.status&~uint32_t(0x3fu<<({a[2]}*6)))|((({imm(a[0])&31}|({a[1]}<<5))&0x3f)<<({a[2]}*6));'];cost=1+int(a[2])
            elif op=='MPYU':
                # 34010ops.hxx MPYU: Rs is zero-extended from FS1 first.
                r=reg(a[1]);i=int(a[1][1:]);code=[f'uint32_t src={value(a[0])}; unsigned w=fw(1); if(w<32)src&=(1u<<w)-1; uint64_t v=uint64_t(src)*{r}; z(v?1:0);']
                code+=([f'{r}=uint32_t(v>>32); {reg(a[1][0]+str(i+1))}=uint32_t(v);'] if i%2==0 else [f'{r}=uint32_t(v);']);cost=21
            elif op in ['MMTM','MMFM']:
                ptr=reg(a[0]);rs=regs(','.join(a[1:]));cost=(2 if op=='MMTM' else 3)+4*len(rs)
                if op=='MMTM':
                    for r in rs:code +=[f'{ptr}-=32; write(0x{pc:08x},{ptr},32,{reg(r)});']
                else:
                    for r in reversed(rs):code +=[f'{reg(r)}=read(0x{pc:08x},{ptr},32); {ptr}+=32;'+(' ++f.check.stack_read_count;' if a[0] in ['SP','A15','B15'] else '')]
            elif op in ['PUSHST','POPST','DINT']:
                if op=='PUSHST':code=[f'push(0x{pc:08x},s.status);'];cost=2
                else:
                    fn='PopStatus' if op=='POPST' else 'Dint';code=[f's.next_original_pc=0x{pc:08x};']
                    if op=='POPST' and M:
                        code.append(f'bool interrupted=irq.PopStatus(0x{pc:08x},0x{n:08x}); result.instructions.push_back({{0x{pc:08x},0x{n:08x},8}}); if(interrupted)throw TaskBodyStop::IrqHandler;')
                    else:
                        code.append(f'if(irq.PopStatus(0x{pc:08x},0x{n:08x}))throw TaskBodyStop::IrqHandler;' if op=='POPST' else f'irq.Dint(0x{pc:08x},0x{n:08x});');code.append(f'result.instructions.push_back({{0x{pc:08x},0x{n:08x},{8 if op=="POPST" else 3}}});')
                    code.append(jump(n));terminal=True
            elif op in ['CALLA','CALLR','JA']:
                t=imm(a[0]);cost=3 if op in ['CALLR','JA'] else 4
                if op!='JA':code=[f'push(0x{pc:08x},0x{n:08x});']
                code +=[f'step(0x{pc:08x},0x{t:08x},{cost});',jump(t)];terminal=True
            elif op=='CALL' and M:
                code=[f'uint32_t target={reg(a[0])}&0xfffffff0; push(0x{pc:08x},0x{n:08x}); step(0x{pc:08x},target,3); goto continuation;'];terminal=True
            elif op=='GETST' and M:code=[f'{reg(a[0])}=s.status;']
            elif op=='PUTST' and M:
                code=[f's.next_original_pc=0x{n:08x}; bool taken=irq.SetStatus(0x{pc:08x},{reg(a[0])}); irq.AdvanceCycles(3);video_update();result.instructions.push_back({{0x{pc:08x},0x{n:08x},3}}); if(taken || irq.CheckBoundary(0x{pc:08x}))throw TaskBodyStop::IrqHandler; goto continuation;'];terminal=True
            elif op=='RETS':
                code=[f'uint32_t sp=s.a[15],v=pop()&0xfffffff0; f.check.returns.push_back({{0x{pc:08x},v,sp}}); step(0x{pc:08x},v,7);'+(' if(subroutine && s.a[15]>subroutine_sp)return;' if boot else '')+' goto continuation;'];terminal=True
            elif op=='GETPC':code=[f'{reg(a[0])}=0x{n:08x};']
            elif op in ['JUMP','EXGPC']:
                r=reg(a[0]);code=[f'uint32_t v={r}&0xfffffff0;']+([f'{r}=0x{n:08x};'] if op=='EXGPC' else [])+[f'step(0x{pc:08x},v,2); goto continuation;'];terminal=True
            elif op in ['DSJS','DSJ']:
                r=reg(a[0]);t=imm(a[1]);code=[f'--{r}; step(0x{pc:08x},{r}?0x{t:08x}:0x{n:08x},{r}?{2 if op=="DSJS" else 3}:{3 if op=="DSJS" else 2}); if({r}){{{jump(t)}}} {jump(n)}'];terminal=True
            elif op in conditions:
                t=imm(a[0]);c=conditions[op];taken=3 if size>=32 else 2;fall=4 if size==48 else (2 if size==32 else 1)
                code=[f'bool taken=({c}); step(0x{pc:08x},taken?0x{t:08x}:0x{n:08x},taken?{taken}:{fall}); if(taken){{{jump(t)}}} {jump(n)}'];terminal=True
            elif op=='NOP':code=[]
            else:raise ValueError(op)
        except (ValueError,AssertionError,IndexError):unsupported.append({'pc':hex(pc),'asm':asm});code=['throw TaskBodyStop::Untranslated;'];terminal=True
        out +=[f'pc_{pc:08x}: {{ // {asm}']
        if M and not boot and pc in ([0xffe13c10,0xffe1a310]+([0xffe2a930] if M else [])):
            out.append(f'if((s.status&0x0fffffff)==0x00200030){{pre(0x{pc:08x});native_block(0x{pc:08x});goto continuation;}}')
        out +=[f'pre(0x{pc:08x}); instruction_next=0x{n:08x};']+code
        if not terminal:out +=[f'step(0x{pc:08x},0x{n:08x},{cost});',jump(n)]
        out.append('}')
    return '\n'.join(out)+'\n',unsupported


MAIN_SHARDS=8   # one 5.5 MB function took 140 s of g++ alone; 8 functions compile in parallel and in less total CPU
LABEL=re.compile(r'pc_([0-9a-f]{8}): \{')


def layout(main,shards=MAIN_SHARDS):
    """How the main-loop graph text is compiled: `shards` member functions over contiguous PC ranges plus the driver
    run by execute_main(). Returns {name: text}: mk5_main_loop_driver.inc, mk5_main_loop_shards.inc (declarations) and
    mk5_main_loop_shard_<k>.cpp. Labels and bodies are the graph's own, in order. A goto to a label of another shard
    returns to the driver: the step() before every goto already left that PC in s.next_original_pc (emit_graph jump),
    so the driver's dispatch reaches the same label, as the graph's own `continuation` switch does.
    An interrupt is entered hundreds of times a frame: where the other graphs' pre() and step() throw
    TaskBodyStop::IrqHandler, a shard returns (pre and step are the runner's pre_interrupt and step_interrupt there) and
    the driver returns to ContinueMain."""
    lines=main.rstrip('\n').split('\n');first=next(i for i,l in enumerate(lines) if LABEL.fullmatch(l.split(' //')[0]))
    head,blocks=lines[:first],[]
    if head[1:3]!=['continuation:','switch(s.next_original_pc){'] or head[-2:]!=['default: throw TaskBodyStop::Untranslated;','}']:
        raise ValueError('main-loop graph head has another form')
    natives=[l for l in head[3:-2] if 'native_block' in l]
    if any(not re.fullmatch(r'case 0x([0-9a-f]{8}): goto pc_\1;',l) for l in head[3:-2] if l not in natives):
        raise ValueError('main-loop graph dispatch has another form')
    for l in lines[first:]:
        m=LABEL.fullmatch(l.split(' //')[0])
        if m:blocks.append((int(m.group(1),16),[l]))
        else:blocks[-1][1].append(l)
    size=sum(len(l)+1 for _,b in blocks for l in b);groups=[[]];done=0
    for block in blocks:
        if done>=size*len(groups)/shards and len(groups)<shards:groups.append([])
        groups[-1].append(block);done+=sum(len(l)+1 for l in block[1])
    note='// Generated offline by tools/mk1/generate.py from the main-loop graph (layout). Fixed native code.'
    out={};driver=[note,'for(;;){','if(interrupt_leave)return;','const uint32_t pc=s.next_original_pc;']
    for k,group in enumerate(groups):
        own={pc for pc,_ in group}
        body=[note,'#include "mk5_task_body.hpp"','#include "mk5_audio_link.hpp"','#include <stdexcept>',
              '#define pre(pc) if(pre_interrupt(pc))return LeaveForInterrupt()',
              '#define step(pc,next,cycles) if(step_interrupt(pc,next,cycles))return LeaveForInterrupt()',
              f'template<> bool Mk5TaskBodyRunner::main_shard<{k}>(){{','const uint32_t entry=s.next_original_pc;',
              'continuation:','switch(s.next_original_pc){',*[f'case 0x{pc:08x}: goto pc_{pc:08x};' for pc in sorted(own)],
              'default: return s.next_original_pc!=entry;   // left this shard, or entered at a PC it does not hold','}']
        for _,block in group:
            body.append(re.sub(r'goto pc_([0-9a-f]{8});',lambda m:m.group(0) if int(m.group(1),16) in own else 'return true;','\n'.join(block)))
        out[f'mk5_main_loop_shard_{k}.cpp']='\n'.join(body+['}'])+'\n'
        driver.append(f'{"else " if k else ""}if(pc>=0x{min(own):08x} && pc<=0x{max(own):08x}){{if(main_shard<{k}>())continue;}}')
    driver+=['switch(pc){',*[l.replace('goto continuation;','continue;') for l in natives],'default: break;','}',
             'throw TaskBodyStop::Untranslated;','}']
    out['mk5_main_loop_driver.inc']='\n'.join(driver)+'\n'
    out['mk5_main_loop_shards.inc']='\n'.join([note,*[f'template<> bool Mk5TaskBodyRunner::main_shard<{k}>();' for k in range(len(groups))]])+'\n'
    return out


def texts(rom,rows=None):
    """{include name: text} of the main-CPU translation of `rom` (the authenticated rev 5.0 image)."""
    if hashlib.sha256(rom).hexdigest()!=PROGRAM['sha256']:raise ValueError('MK1 program image differs from rev 5.0')
    if rows is None:rows,_=scan(rom)
    out={};report={}
    for name,main in (('mk5_main_loop_graph.inc',True),('mk5_task_body_graph.inc',False)):
        text,unsupported=emit_graph(graph_rows(rows,table('main_loop' if main else 'task_body')),main)
        out[name]=text;report[name]=dict(unsupported=len(unsupported))
    text,unsupported=emit_graph(graph_rows(rows,table('boot')),True,boot=True)
    out['mk5_boot_graph.inc']=text;report['mk5_boot_graph.inc']=dict(unsupported=len(unsupported),forms=sorted({u['asm'].split()[0] for u in unsupported}))
    return out,report
