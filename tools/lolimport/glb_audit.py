
import json, struct, sys, re
from collections import defaultdict

path = sys.argv[1]
raw = open(path,'rb').read()
assert raw[:4]==b'glTF'
off=12; js=None; bin_=None
while off < len(raw):
    ln, ty = struct.unpack_from('<II', raw, off); off+=8
    chunk = raw[off:off+ln]; off+=ln
    if ty==0x4E4F534A: js=json.loads(chunk.decode('utf-8'))
    elif ty==0x004E4942: bin_=chunk

def acc_vals(i):
    a=js['accessors'][i]
    bv=js['bufferViews'][a['bufferView']]
    ctype={5120:'b',5121:'B',5122:'h',5123:'H',5125:'I',5126:'f'}[a['componentType']]
    ncomp={'SCALAR':1,'VEC2':2,'VEC3':3,'VEC4':4,'MAT4':16}[a['type']]
    size=struct.calcsize(ctype)
    stride=bv.get('byteStride') or size*ncomp
    base=bv.get('byteOffset',0)+a.get('byteOffset',0)
    out=[]
    for k in range(a['count']):
        o=base+k*stride
        out.append(struct.unpack_from('<'+ctype*ncomp, bin_, o))
    return out

mats=js.get('materials',[])
texs=js.get('textures',[])
imgs=js.get('images',[])
rows=[]
for mi,mesh in enumerate(js['meshes']):
    for pi,p in enumerate(mesh['primitives']):
        attrs=p['attributes']
        pos=js['accessors'][attrs['POSITION']]
        posv=acc_vals(attrs['POSITION'])
        xs=[v[0] for v in posv]; ys=[v[1] for v in posv]; zs=[v[2] for v in posv]
        span=(max(xs)-min(xs), max(ys)-min(ys), max(zs)-min(zs))
        uvr=None
        if 'TEXCOORD_0' in attrs:
            uv=acc_vals(attrs['TEXCOORD_0'])
            us=[v[0] for v in uv]; vs=[v[1] for v in uv]
            uvr=(min(us),max(us),min(vs),max(vs))
        nidx=js['accessors'][p['indices']]['count'] if 'indices' in p else 0
        m=mats[p['material']] if 'material' in p else {}
        name=m.get('name','?')
        pbr=m.get('pbrMetallicRoughness',{})
        bct=pbr.get('baseColorTexture')
        texname=''
        if bct is not None:
            t=texs[bct['index']]
            if 'source' in t: texname=imgs[t['source']].get('name','')
        rows.append(dict(name=name, prim=pi, span=span, uv=uvr, tris=nidx//3, tex=texname,
                         alpha=m.get('alphaMode','OPAQUE'),
                         center=((min(xs)+max(xs))/2,(min(ys)+max(ys))/2,(min(zs)+max(zs))/2)))

print('total prims', len(rows))
print()
hdr='%-52s %5s %9s %9s %9s %8s %8s %8s %9s  %s'%('material','prim','spanX','spanY','spanZ','uvU','uvV','texels/u','tris','tex')
print(hdr)
for r in sorted(rows, key=lambda r:(r['name'],r['prim'])):
    sx,sy,sz=r['span']
    tu=tv=0
    if r['uv']:
        du=r['uv'][1]-r['uv'][0]; dv=r['uv'][3]-r['uv'][2]
        tu = 1024.0/ (sx/du) if du>1e-9 and sx>1e-9 else 0
        tv = 1024.0/ (sy/dv) if dv>1e-9 and sy>1e-9 else 0
    print('%-62s %5d %9.2f %9.2f %9.2f %8.3f %8.3f %8.1f %9d  center=(%7.1f,%6.1f,%7.1f) %s'%(
        r['name'], r['prim'], sx,sy,sz, r['uv'][0] if r['uv'] else 0, r['uv'][3] if r['uv'] else 0, tu, r['tris'], r['center'][0], r['center'][1], r['center'][2], r['tex'][:60]))

print()
agg=defaultdict(lambda:[0,0,0,0])
for r in rows:
    a=agg[r['name']]
    a[0]+=1; a[1]+=r['tris']
    a[2]+=r['span'][0]; a[3]+=r['span'][1]
print('per-material aggregate (count, tris, sumSpanX, sumSpanY):')
for k,v in sorted(agg.items(), key=lambda kv:-kv[1][1]):
    print('  %-52s n=%-3d tris=%-8d spanX=%-8.1f spanY=%.1f'%(k[:52],v[0],v[1],v[2],v[3]))
