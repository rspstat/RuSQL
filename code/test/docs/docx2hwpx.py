"""A Word .docx -> .hwpx, read straight from the docx XML (made for RuSQL_통합문서_v29.docx and RuSQL_임시문서.docx).
Why not Hangul itself: Hangul's COM Open cannot read a .docx, and the ODT detour (Word -> ODT -> Hangul) loses bold, heading numbers and the table look.
usage:  DOCX_PATH=<file.docx> py code/test/docs/docx2hwpx.py <out.hwpx>      then let Hangul open it and save it again as HWPX (it adds its own layout data):
        HwpObject.Open(out, "HWPX", ""); HwpObject.SaveAs(final, "HWPX", "")
Needs: the D06 hwpx of the docs/deliverables folder tree as the package template (fonts list, scripts, container files).
Both Word files were deleted from docs/deliverables on 2026-11-11 (the .hwpx files are the working documents now); they are in git history (commit 05ab639).
RuSQL_임시문서.docx (one 109-row Gantt table, 6.5 pt text) was converted with ROW_MIN=2390 CELL_LINE_PCT=188 TBL_BREAK=CELL: 5 pages like Word, the same rows on every page.

The look is calibrated against Word's own PDF of the docx (2026-11-11): Word could not use the Noto Sans KR the docx names (Hangul draws that variable font only as Thin),
so it laid the text out with Gulim-like widths (Hangul syllable 1.00 em, blank 0.333 em) and a line pitch of 1.878 x the size. The defaults below reproduce that with the
Hancom Gothic face: character spacing +7 % (Hangul), +1 % (Latin), -8 % (other), 'useFontSpace' (the blank takes the font's own width), line spacing 145.7 %, no grid snapping.
Result: 52 pages like Word, every heading on the same page, all 25 pictures the same size.
env overrides: FONT_FACE, LINE_PCT, CELL_LINE_PCT (inside table cells), SP_H, SP_L, SP_O, RATIO_H, RATIO_L, USE_FONT_SPACE, BREAK_L, TBL_X, FLH,
               ROW_MIN (least table row height, HWPUNIT), TBL_BREAK (CELL / TABLE), DOC_TITLE, DOC_PREVIEW, DOCX_PATH
"""
import zipfile, re, os, sys, glob, io
from lxml import etree
from xml.sax.saxutils import escape

SP = os.path.dirname(os.path.abspath(__file__))
D = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(SP))), 'docs', 'deliverables')   # <repo>/docs/deliverables
DOCX = os.environ.get('DOCX_PATH') or sys.exit('set DOCX_PATH to the .docx to convert (the Word files of docs/deliverables were deleted on 2026-11-11; see git history)')
TEMPLATE = glob.glob(os.path.join(D, '2026_졸업작품_산출물_개정', 'hwpx', '작성본', '*-D06-*.hwpx'))[0]
FACE = os.environ.get('FONT_FACE', '한컴 고딕')
LINE_PCT = float(os.environ.get('LINE_PCT', '145.7'))
CELL_PCT = float(os.environ.get('CELL_LINE_PCT', str(LINE_PCT)))    # line spacing of the paragraphs inside table cells
cur_pct = [LINE_PCT]                                                   # the value the paragraphs written right now get
FLH = os.environ.get('FLH', '1')
TBL_X = int(os.environ.get('TBL_X', '0'))      # horizontal offset of the tables (HWPUNIT)
BREAK_L = os.environ.get('BREAK_L', 'BREAK_WORD')   # Latin words: KEEP_WORD (break at blanks and slashes) or BREAK_WORD (anywhere)
UFS = os.environ.get('USE_FONT_SPACE', '1')      # charPr useFontSpace: the blank takes the font's own width
SP_H = int(float(os.environ.get('SP_H', '7')))     # character spacing (자간) in % of the em: Hangul / Latin / the rest
SP_L = int(float(os.environ.get('SP_L', '1')))
SP_O = int(float(os.environ.get('SP_O', '-8')))
RATIO_H = int(float(os.environ.get('RATIO_H', '100')))   # character width (장평) of Hangul text, %
RATIO_L = int(float(os.environ.get('RATIO_L', '100')))   # ... of Latin text, %
OUT = sys.argv[1]

W = 'http://schemas.openxmlformats.org/wordprocessingml/2006/main'
WP = 'http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing'
A = 'http://schemas.openxmlformats.org/drawingml/2006/main'
R = 'http://schemas.openxmlformats.org/officeDocument/2006/relationships'
PIC = 'http://schemas.openxmlformats.org/drawingml/2006/picture'
HH = '{http://www.hancom.co.kr/hwpml/2011/head}'
q = lambda t: '{%s}%s' % (W, t)
TW = 5            # 1 twip = 5 HWPUNIT (1 pt = 100)
DEFAULT_SZ = 20   # Word's default size when nothing says otherwise: 10 pt (half-points)

zd = zipfile.ZipFile(DOCX)
doc = etree.fromstring(zd.read('word/document.xml'))
body = doc.find(q('body'))
sty = etree.fromstring(zd.read('word/styles.xml'))
rels = {r.get('Id'): r.get('Target') for r in etree.fromstring(zd.read('word/_rels/document.xml.rels'))}


# ------------------------------------------------------------------ styles
def style_props(style_id):
    out = dict(before=0, after=0, left=0, right=0, first=0, jc=None, outline=None, rpr={})
    for s in sty.findall(q('style')):
        if s.get(q('styleId')) == style_id:
            sp = s.find(q('pPr'))
            if sp is not None:
                sps = sp.find(q('spacing'))
                if sps is not None:
                    out['before'] = int(sps.get(q('before'), 0)); out['after'] = int(sps.get(q('after'), 0))
                ol = sp.find(q('outlineLvl'))
                if ol is not None: out['outline'] = int(ol.get(q('val')))
            out['rpr'] = read_rpr(s.find(q('rPr')))
    return out


def read_rpr(rpr):
    d = {}
    if rpr is None:
        return d
    if rpr.find(q('b')) is not None and rpr.find(q('b')).get(q('val')) not in ('0', 'false'): d['b'] = True
    if rpr.find(q('i')) is not None and rpr.find(q('i')).get(q('val')) not in ('0', 'false'): d['i'] = True
    c = rpr.find(q('color'))
    if c is not None and c.get(q('val')) not in (None, 'auto'): d['color'] = '#' + c.get(q('val')).upper()
    s = rpr.find(q('sz'))
    if s is not None: d['sz'] = int(s.get(q('val')))
    return d


STYLES = {sid: style_props(sid) for sid in ('Heading1', 'Heading2', 'Heading3')}


# ------------------------------------------------------------------ registries for the header lists
class Reg:
    def __init__(self, first_id=0):
        self.items = {}; self.first = first_id

    def get(self, key):
        if key not in self.items:
            self.items[key] = self.first + len(self.items)
        return self.items[key]


char_reg = Reg(0); para_reg = Reg(0); bf_reg = Reg(3)    # border fill ids 1 and 2 are fixed (none / table frame)


def charpr_id(rd):
    return char_reg.get((int(round(rd.get('sz', DEFAULT_SZ) * 50)), bool(rd.get('b')), bool(rd.get('i')), rd.get('color', '#000000')))


def cell_bf(fill):
    return bf_reg.get(('cell', fill))


def pbdr_bf():
    return bf_reg.get(('pbdr',))


def parse_ppr(p):
    ppr = p.find(q('pPr'))
    d = dict(style=None, before=0, after=0, left=0, right=0, first=0, jc='LEFT', pbdr=None, mark=None)
    if ppr is None:
        return d
    ps = ppr.find(q('pStyle'))
    if ps is not None:
        d['style'] = ps.get(q('val'))
        st = STYLES.get(d['style'])
        if st:
            d['before'], d['after'] = st['before'], st['after']
    sp = ppr.find(q('spacing'))
    if sp is not None:
        if sp.get(q('before')) is not None: d['before'] = int(sp.get(q('before')))
        if sp.get(q('after')) is not None: d['after'] = int(sp.get(q('after')))
        assert sp.get(q('line')) is None, 'a paragraph with its own line spacing: not handled'
    ind = ppr.find(q('ind'))
    if ind is not None:
        d['left'] = int(ind.get(q('left'), 0)); d['right'] = int(ind.get(q('right'), 0)); d['first'] = int(ind.get(q('firstLine'), 0))
        assert ind.get(q('hanging')) is None
    jc = ppr.find(q('jc'))
    if jc is not None:
        d['jc'] = {'center': 'CENTER', 'left': 'LEFT', 'right': 'RIGHT', 'both': 'JUSTIFY'}[jc.get(q('val'))]
    pb = ppr.find(q('pBdr'))
    if pb is not None:
        bt = pb.find(q('bottom'))
        assert bt is not None and len(pb) == 1
        d['pbdr'] = (bt.get(q('val')), int(bt.get(q('sz'))), int(bt.get(q('space'))), bt.get(q('color')))
    d['mark'] = read_rpr(ppr.find(q('rPr')))
    return d


# ------------------------------------------------------------------ pictures
bin_items = []        # (id, source name inside the docx, bytes)


def png_size(data):
    import struct
    return struct.unpack('>II', data[16:24])


def pic_xml(drawing, uid):
    inl = drawing.find('{%s}inline' % WP)
    ext = inl.find('{%s}extent' % WP)
    cx, cy = int(ext.get('cx')) // 127, int(ext.get('cy')) // 127
    blip = drawing.find('.//{%s}blip' % A)
    target = rels[blip.get('{%s}embed' % R)]
    data = zd.read('word/' + target)
    pxw, pxh = png_size(data)
    bid = 'image%d' % (len(bin_items) + 1)
    bin_items.append((bid, target, data))
    ow, oh = pxw * 75, pxh * 75
    return (f'<hp:pic id="{1900000000 + uid * 2 + 1}" zOrder="{uid}" numberingType="PICTURE" textWrap="TOP_AND_BOTTOM" textFlow="BOTH_SIDES" lock="0" dropcapstyle="None" href="" groupLevel="0" instid="{1900000000 + uid * 2 + 2}" reverse="0">'
            f'<hp:offset x="0" y="0"/><hp:orgSz width="{ow}" height="{oh}"/><hp:curSz width="{cx}" height="{cy}"/><hp:flip horizontal="0" vertical="0"/>'
            f'<hp:rotationInfo angle="0" centerX="{cx // 2}" centerY="{cy // 2}" rotateimage="1"/>'
            f'<hp:renderingInfo><hc:transMatrix e1="1" e2="0" e3="0" e4="0" e5="1" e6="0"/><hc:scaMatrix e1="{cx / ow!r}" e2="0" e3="0" e4="0" e5="{cy / oh!r}" e6="0"/><hc:rotMatrix e1="1" e2="0" e3="0" e4="0" e5="1" e6="0"/></hp:renderingInfo>'
            f'<hc:img binaryItemIDRef="{bid}" bright="0" contrast="0" effect="REAL_PIC" alpha="0"/>'
            f'<hp:imgRect><hc:pt0 x="0" y="0"/><hc:pt1 x="{ow}" y="0"/><hc:pt2 x="{ow}" y="{oh}"/><hc:pt3 x="0" y="{oh}"/></hp:imgRect><hp:imgClip left="0" right="{ow}" top="0" bottom="{oh}"/>'
            f'<hp:inMargin left="0" right="0" top="0" bottom="0"/><hp:imgDim dimwidth="{ow}" dimheight="{oh}"/><hp:effects/>'
            f'<hp:sz width="{cx}" widthRelTo="ABSOLUTE" height="{cy}" heightRelTo="ABSOLUTE" protect="0"/>'
            f'<hp:pos treatAsChar="1" affectLSpacing="0" flowWithText="1" allowOverlap="0" holdAnchorAndSO="0" vertRelTo="PARA" horzRelTo="PARA" vertAlign="TOP" horzAlign="LEFT" vertOffset="0" horzOffset="0"/>'
            f'<hp:outMargin left="0" right="0" top="0" bottom="0"/></hp:pic>')


# ------------------------------------------------------------------ paragraphs
uid = [0]


def para_xml(p, first_ctrl='', force_pagebreak=False, prev_after=0):
    d = parse_ppr(p)
    after_out = d['after']
    if isinstance(prev_after, tuple):        # the paragraph above had a bottom rule: its 'after' counts from the rule, and the rule's own distance + thickness is space too
        d['before'] = prev_after[2] + max(d['before'], prev_after[1])
    else:
        d['before'] = max(0, d['before'] - prev_after)
    if d['pbdr']:                            # Hangul draws the rule at the paragraph's end + offset without making room: the room goes to the next paragraph
        after_out = ('rule', d['after'], d['pbdr'][2] * 20 + int(d['pbdr'][1] * 2.5))
        d['after'] = 0
    base = {}
    if d['style'] in STYLES:
        base = dict(STYLES[d['style']]['rpr'])
    pagebreak = force_pagebreak
    segs = []        # ('t', charPrId, text) or ('pic', xml)
    seen_content = False
    for r in p.findall(q('r')):
        rd = dict(base); rd.update(read_rpr(r.find(q('rPr'))))
        cid = charpr_id(rd)
        for ch in r:
            n = etree.QName(ch).localname
            if n == 't':
                if ch.text:
                    segs.append(('t', cid, ch.text)); seen_content = True
            elif n == 'br':
                assert ch.get(q('type')) == 'page'
                assert not seen_content, 'a page break in the middle of a paragraph'
                pagebreak = True
            elif n == 'drawing':
                uid[0] += 1
                segs.append(('pic', cid, pic_xml(ch, uid[0]))); seen_content = True
            elif n in ('rPr', 'lastRenderedPageBreak'):
                pass
            else:
                raise SystemExit('unhandled run child: ' + n)
    mark = dict(base); mark.update(d['mark'])
    pbf = pbdr_bf() if d['pbdr'] else 1
    pid = para_reg.get((d['jc'], d['before'], d['after'], d['left'], d['right'], d['first'], d['pbdr'], cur_pct[0]))
    out = [f'<hp:p id="0" paraPrIDRef="{pid}" styleIDRef="{ {"Heading1": 1, "Heading2": 2, "Heading3": 3}.get(d["style"], 0) }" pageBreak="{1 if pagebreak else 0}" columnBreak="0" merged="0">']
    if not segs:
        out.append(f'<hp:run charPrIDRef="{charpr_id(mark)}">{first_ctrl}<hp:t/></hp:run>')
    else:
        first = True
        for s in segs:
            if s[0] == 't':
                out.append(f'<hp:run charPrIDRef="{s[1]}">{first_ctrl if first else ""}<hp:t>{escape(s[2])}</hp:t></hp:run>')
            else:
                out.append(f'<hp:run charPrIDRef="{s[1]}">{first_ctrl if first else ""}{s[2]}<hp:t/></hp:run>')
            first = False
    out.append('</hp:p>')
    return ''.join(out), after_out


# ------------------------------------------------------------------ tables
tbl_uid = [0]
ROW_MIN = int(os.environ.get('ROW_MIN', '2300'))    # the least row height (cell margins + one line); Hangul grows the row to its text


def table_xml(tbl):
    tbl_uid[0] += 1
    grid = [int(g.get(q('w'))) for g in tbl.find(q('tblGrid'))]
    rows = tbl.findall(q('tr'))
    cells = {}      # (r, c) -> dict(tc, rowspan)
    owner = {}      # column -> (row index of the restarting cell)
    for ri, tr in enumerate(rows):
        ci = 0
        for tc in tr.findall(q('tc')):
            vm = tc.find(q('tcPr')).find(q('vMerge'))
            gs = tc.find(q('tcPr')).find(q('gridSpan'))
            span = int(gs.get(q('val'))) if gs is not None else 1
            if vm is not None and vm.get(q('val')) != 'restart':
                cells[owner[ci]]['rowspan'] += 1
            else:
                cells[(ri, ci)] = dict(tc=tc, rowspan=1, span=span)
                if vm is not None:
                    owner[ci] = (ri, ci)
            ci += span
    tblw = sum(grid) * TW
    # Word: a row without <w:cantSplit/> may break across pages; Hangul decides per table: any such row -> cells may break ('CELL'), else only between rows ('TABLE')
    can_split = any(tr.find(q('trPr')) is None or tr.find(q('trPr')).find(q('cantSplit')) is None for tr in rows)
    out = [f'<hp:tbl id="{2095000000 + tbl_uid[0]}" zOrder="{tbl_uid[0]}" numberingType="TABLE" textWrap="TOP_AND_BOTTOM" textFlow="BOTH_SIDES" lock="0" dropcapstyle="None" pageBreak="{os.environ.get("TBL_BREAK") or ("CELL" if can_split else "TABLE")}" repeatHeader="1" rowCnt="{len(rows)}" colCnt="{len(grid)}" cellSpacing="0" borderFillIDRef="2" noAdjust="0">',
           f'<hp:sz width="{tblw}" widthRelTo="ABSOLUTE" height="{ROW_MIN * len(rows)}" heightRelTo="ABSOLUTE" protect="0"/>',
           f'<hp:pos treatAsChar="0" affectLSpacing="0" flowWithText="1" allowOverlap="0" holdAnchorAndSO="0" vertRelTo="PARA" horzRelTo="COLUMN" vertAlign="TOP" horzAlign="LEFT" vertOffset="0" horzOffset="{TBL_X}"/>',
           '<hp:outMargin left="0" right="0" top="0" bottom="0"/><hp:inMargin left="0" right="0" top="0" bottom="0"/>']
    cur_pct[0] = CELL_PCT
    for ri, tr in enumerate(rows):
        header = tr.find(q('trPr')) is not None and tr.find(q('trPr')).find(q('tblHeader')) is not None
        out.append('<hp:tr>')
        for ci in range(len(grid)):
            if (ri, ci) not in cells:
                continue
            c = cells[(ri, ci)]; tc = c['tc']
            pr = tc.find(q('tcPr'))
            fill = '#' + pr.find(q('shd')).get(q('fill')).upper()
            mar = pr.find(q('tcMar'))
            ml, mr, mt, mb = (int(mar.find(q(k)).get(q('w'))) * TW for k in ('left', 'right', 'top', 'bottom'))
            width = int(pr.find(q('tcW')).get(q('w'))) * TW
            valign = {'center': 'CENTER', 'top': 'TOP', 'bottom': 'BOTTOM'}[pr.find(q('vAlign')).get(q('val'))]
            out.append(f'<hp:tc name="" header="{1 if header else 0}" hasMargin="1" protect="0" editable="0" dirty="0" borderFillIDRef="{cell_bf(fill)}">')
            out.append(f'<hp:subList id="" textDirection="HORIZONTAL" lineWrap="BREAK" vertAlign="{valign}" linkListIDRef="0" linkListNextIDRef="0" textWidth="0" textHeight="0" hasTextRef="0" hasNumRef="0">')
            pa = 0
            for p in tc.findall(q('p')):
                px, pa = para_xml(p, prev_after=pa)
                out.append(px)
            out.append('</hp:subList>')
            out.append(f'<hp:cellAddr colAddr="{ci}" rowAddr="{ri}"/><hp:cellSpan colSpan="{c["span"]}" rowSpan="{c["rowspan"]}"/><hp:cellSz width="{width}" height="{ROW_MIN * c["rowspan"]}"/><hp:cellMargin left="{ml}" right="{mr}" top="{mt}" bottom="{mb}"/></hp:tc>')
        out.append('</hp:tr>')
    cur_pct[0] = LINE_PCT
    out.append('</hp:tbl>')
    return ''.join(out)


# ------------------------------------------------------------------ the body
sect = body.find(q('sectPr'))
pg = sect.find(q('pgSz')); mg = sect.find(q('pgMar'))
PW, PH = int(pg.get(q('w'))) * TW, int(pg.get(q('h'))) * TW
M_TOP, M_BOT, M_L, M_R = (int(mg.get(q(k))) * TW for k in ('top', 'bottom', 'left', 'right'))
M_HDR, M_FTR = int(mg.get(q('header'))) * TW, int(mg.get(q('footer'))) * TW
HIDE_FIRST = 1 if sect.find(q('titlePg')) is not None else 0     # a cover page without footer / page number only when the docx has a 'different first page'

FOOT_SZ = 18   # the footer's page number is 9 pt
foot_char = charpr_id(dict(sz=FOOT_SZ))
foot_para = para_reg.get(('CENTER', 0, 0, 0, 0, 0, None, LINE_PCT))
SEC_CTRL = (
    '<hp:ctrl><hp:colPr id="" type="NEWSPAPER" layout="LEFT" colCount="1" sameSz="1" sameGap="0"/></hp:ctrl>'
    f'<hp:secPr id="" textDirection="HORIZONTAL" spaceColumns="1134" tabStop="8000" tabStopVal="4000" tabStopUnit="HWPUNIT" outlineShapeIDRef="0" memoShapeIDRef="0" textVerticalWidthHead="0" masterPageCnt="0">'
    '<hp:grid lineGrid="0" charGrid="0" wonggojiFormat="0"/><hp:startNum pageStartsOn="BOTH" page="0" pic="0" tbl="0" equation="0"/>'
    f'<hp:visibility hideFirstHeader="0" hideFirstFooter="{HIDE_FIRST}" hideFirstMasterPage="0" border="SHOW_ALL" fill="SHOW_ALL" hideFirstPageNum="{HIDE_FIRST}" hideFirstEmptyLine="0" showLineNumber="0"/>'
    '<hp:lineNumberShape restartType="0" countBy="0" distance="0" startNumber="0"/>'
    f'<hp:pagePr landscape="WIDELY" width="{PW}" height="{PH}" gutterType="LEFT_ONLY"><hp:margin header="{M_TOP - M_HDR}" footer="{M_BOT - M_FTR}" gutter="0" left="{M_L}" right="{M_R}" top="{M_HDR}" bottom="{M_FTR}"/></hp:pagePr>'
    '<hp:footNotePr><hp:autoNumFormat type="DIGIT" userChar="" prefixChar="" suffixChar=")" supscript="0"/><hp:noteLine length="-1" type="SOLID" width="0.12 mm" color="#000000"/><hp:noteSpacing betweenNotes="283" belowLine="567" aboveLine="850"/><hp:numbering type="CONTINUOUS" newNum="1"/><hp:placement place="EACH_COLUMN" beneathText="0"/></hp:footNotePr>'
    '<hp:endNotePr><hp:autoNumFormat type="DIGIT" userChar="" prefixChar="" suffixChar=")" supscript="0"/><hp:noteLine length="14692344" type="SOLID" width="0.12 mm" color="#000000"/><hp:noteSpacing betweenNotes="0" belowLine="567" aboveLine="850"/><hp:numbering type="CONTINUOUS" newNum="1"/><hp:placement place="END_OF_DOCUMENT" beneathText="0"/></hp:endNotePr>'
    '<hp:pageBorderFill type="BOTH" borderFillIDRef="1" textBorder="CONTENT" headerInside="0" footerInside="0" fillArea="PAPER"><hp:offset left="1417" right="1417" top="1417" bottom="1417"/></hp:pageBorderFill>'
    '<hp:pageBorderFill type="EVEN" borderFillIDRef="1" textBorder="PAPER" headerInside="0" footerInside="0" fillArea="PAPER"><hp:offset left="1417" right="1417" top="1417" bottom="1417"/></hp:pageBorderFill>'
    '<hp:pageBorderFill type="ODD" borderFillIDRef="1" textBorder="PAPER" headerInside="0" footerInside="0" fillArea="PAPER"><hp:offset left="1417" right="1417" top="1417" bottom="1417"/></hp:pageBorderFill></hp:secPr>'
    '<hp:ctrl><hp:pageNum pos="NONE" formatType="DIGIT" sideChar="-"/></hp:ctrl>'
    f'<hp:ctrl><hp:footer id="0" applyPageType="BOTH"><hp:subList id="" textDirection="HORIZONTAL" lineWrap="BREAK" vertAlign="BOTTOM" linkListIDRef="0" linkListNextIDRef="0" textWidth="0" textHeight="0" hasTextRef="0" hasNumRef="0">'
    f'<hp:p id="0" paraPrIDRef="{foot_para}" styleIDRef="0" pageBreak="0" columnBreak="0" merged="0"><hp:run charPrIDRef="{foot_char}"><hp:ctrl><hp:autoNum num="1" numType="PAGE"><hp:autoNumFormat type="DIGIT" userChar="" prefixChar="" suffixChar="" supscript="0"/></hp:autoNum></hp:ctrl></hp:run></hp:p></hp:subList></hp:footer></hp:ctrl>'
)

parts = []
first = True
host_char = charpr_id(dict(sz=2))
host_para = para_reg.get(('LEFT', 0, 0, 0, 0, 0, None, LINE_PCT))
body_after = 0
for ch in body:
    n = etree.QName(ch).localname
    if n == 'sectPr':
        continue
    if n == 'p':
        px, body_after = para_xml(ch, first_ctrl=SEC_CTRL if first else '', prev_after=body_after)
        parts.append(px)
    elif n == 'tbl':
        assert not first
        body_after = 0
        parts.append(f'<hp:p id="0" paraPrIDRef="{host_para}" styleIDRef="0" pageBreak="0" columnBreak="0" merged="0"><hp:run charPrIDRef="{host_char}">{table_xml(ch)}<hp:t/></hp:run></hp:p>')
    first = False

# ------------------------------------------------------------------ header.xml
zt = zipfile.ZipFile(TEMPLATE)
hdr = etree.fromstring(zt.read('Contents/header.xml'))
ref = hdr.find(HH + 'refList')
NSDECL = ' '.join(f'xmlns:{k}="{v}"' for k, v in hdr.nsmap.items())


def frag(xml):
    return etree.fromstring(f'<root {NSDECL}>{xml}</root>')[0]


def font_xml(lang):
    return (f'<hh:fontface lang="{lang}" fontCnt="1"><hh:font id="0" face="{escape(FACE)}" type="TTF" isEmbedded="0">'
            '<hh:typeInfo familyType="FCAT_GOTHIC" weight="5" proportion="3" contrast="2" strokeVariation="0" armStyle="0" letterform="2" midline="0" xHeight="4"/></hh:font></hh:fontface>')


def border_xml(i, kind, fill=None):
    none = '<hh:{s}Border type="NONE" width="0.1 mm" color="#000000"/>'
    solid = '<hh:{s}Border type="SOLID" width="0.1 mm" color="#AAAAAA"/>'
    if kind == 'none':
        b = ''.join(none.format(s=s) for s in ('left', 'right', 'top', 'bottom')); brush = 'none'
    elif kind == 'table':
        b = ''.join(solid.format(s=s) for s in ('left', 'right', 'top', 'bottom')); brush = 'none'
    elif kind == 'cell':
        b = ''.join(solid.format(s=s) for s in ('left', 'right', 'top', 'bottom')); brush = fill
    else:   # paragraph rule: only the bottom line (1 pt black)
        b = (none.format(s='left') + none.format(s='right') + none.format(s='top') + '<hh:bottomBorder type="SOLID" width="0.4 mm" color="#000000"/>'); brush = 'none'
    return (f'<hh:borderFill id="{i}" threeD="0" shadow="0" centerLine="NONE" breakCellSeparateLine="0"><hh:slash type="NONE" Crooked="0" isCounter="0"/><hh:backSlash type="NONE" Crooked="0" isCounter="0"/>{b}'
            f'<hh:diagonal type="SOLID" width="0.1 mm" color="#000000"/><hc:fillBrush><hc:winBrush faceColor="{brush}" hatchColor="#000000" alpha="0"/></hc:fillBrush></hh:borderFill>')


def charpr_xml(i, key):
    height, bold, italic, color = key
    return (f'<hh:charPr id="{i}" height="{height}" textColor="{color}" shadeColor="none" useFontSpace="{UFS}" useKerning="0" symMark="NONE" borderFillIDRef="1">'
            '<hh:fontRef hangul="0" latin="0" hanja="0" japanese="0" other="0" symbol="0" user="0"/><hh:ratio hangul="' + str(RATIO_H) + '" latin="' + str(RATIO_L) + '" hanja="' + str(RATIO_H) + '" japanese="' + str(RATIO_H) + '" other="' + str(RATIO_L) + '" symbol="' + str(RATIO_L) + '" user="' + str(RATIO_L) + '"/>'
            '<hh:spacing hangul="' + str(SP_H) + '" latin="' + str(SP_L) + '" hanja="' + str(SP_H) + '" japanese="' + str(SP_H) + '" other="' + str(SP_O) + '" symbol="' + str(SP_O) + '" user="' + str(SP_L) + '"/><hh:relSz hangul="100" latin="100" hanja="100" japanese="100" other="100" symbol="100" user="100"/>'
            '<hh:offset hangul="0" latin="0" hanja="0" japanese="0" other="0" symbol="0" user="0"/>' + ('<hh:italic/>' if italic else '') + ('<hh:bold/>' if bold else '') +
            '<hh:underline type="NONE" shape="SOLID" color="#000000"/><hh:strikeout shape="NONE" color="#000000"/><hh:outline type="NONE"/><hh:shadow type="NONE" color="#B2B2B2" offsetX="10" offsetY="10"/></hh:charPr>')


def parapr_xml(i, key):
    jc, before, after, left, right, first, pbdr, pct = key
    bf = pbdr_bf() if pbdr else 1
    off_b = (pbdr[2] * 100) if pbdr else 0
    m = (f'<hh:margin><hc:intent value="{first * TW}" unit="HWPUNIT"/><hc:left value="{left * TW}" unit="HWPUNIT"/><hc:right value="{right * TW}" unit="HWPUNIT"/>'
         f'<hc:prev value="{before * TW}" unit="HWPUNIT"/><hc:next value="{after * TW}" unit="HWPUNIT"/></hh:margin><hh:lineSpacing type="PERCENT" value="{int(pct)}" unit="HWPUNIT"/>')
    return (f'<hh:paraPr id="{i}" tabPrIDRef="0" condense="0" fontLineHeight="{FLH}" snapToGrid="0" suppressLineNumbers="0" checked="0" textDir="AUTO"><hh:align horizontal="{jc}" vertical="BASELINE"/>'
            f'<hh:heading type="NONE" idRef="0" level="0"/><hh:breakSetting breakLatinWord="{BREAK_L}" breakNonLatinWord="BREAK_WORD" widowOrphan="0" keepWithNext="0" keepLines="0" pageBreakBefore="0" lineWrap="BREAK"/>'
            '<hh:autoSpacing eAsianEng="0" eAsianNum="0"/>'
            f'<hp:switch><hp:case hp:required-namespace="http://www.hancom.co.kr/hwpml/2016/HwpUnitChar">{m}</hp:case><hp:default>{m}</hp:default></hp:switch>'
            f'<hh:border borderFillIDRef="{bf}" offsetLeft="0" offsetRight="0" offsetTop="0" offsetBottom="{off_b}" connect="0" ignoreMargin="0"/></hh:paraPr>')


def styles_xml():
    def st(i, typ, name, eng, pp, cp):
        return f'<hh:style id="{i}" type="{typ}" name="{name}" engName="{eng}" paraPrIDRef="{pp}" charPrIDRef="{cp}" nextStyleIDRef="{i}" langID="1042" lockForm="0"/>'
    items = [st(0, 'PARA', '바탕글', 'Normal', 0, 0)]
    for k, (nm, en) in enumerate((('제목 1', 'Heading 1'), ('제목 2', 'Heading 2'), ('제목 3', 'Heading 3')), 1):
        items.append(st(k, 'PARA', nm, en, 0, 0))
    return f'<hh:styles itemCnt="{len(items)}">' + ''.join(items) + '</hh:styles>'


# border fills: 1 none, 2 table frame, 3.. cells / paragraph rule
bfs = [border_xml(1, 'none'), border_xml(2, 'table')]
for key, i in sorted(bf_reg.items.items(), key=lambda kv: kv[1]):
    bfs.append(border_xml(i, 'cell', key[1]) if key[0] == 'cell' else border_xml(i, 'rule'))
new_lists = {
    'fontfaces': f'<hh:fontfaces itemCnt="7">' + ''.join(font_xml(l) for l in ('HANGUL', 'LATIN', 'HANJA', 'JAPANESE', 'OTHER', 'SYMBOL', 'USER')) + '</hh:fontfaces>',
    'borderFills': f'<hh:borderFills itemCnt="{len(bfs)}">' + ''.join(bfs) + '</hh:borderFills>',
    'charProperties': f'<hh:charProperties itemCnt="{len(char_reg.items)}">' + ''.join(charpr_xml(i, k) for k, i in sorted(char_reg.items.items(), key=lambda kv: kv[1])) + '</hh:charProperties>',
    'paraProperties': f'<hh:paraProperties itemCnt="{len(para_reg.items)}">' + ''.join(parapr_xml(i, k) for k, i in sorted(para_reg.items.items(), key=lambda kv: kv[1])) + '</hh:paraProperties>',
    'styles': styles_xml(),
}
for name, xml in new_lists.items():
    old = ref.find(HH + name)
    ref.replace(old, frag(xml))
hdr.set('secCnt', '1')
header_bytes = etree.tostring(hdr, xml_declaration=True, encoding='UTF-8', standalone=True)

# ------------------------------------------------------------------ section0.xml
sec_root = etree.fromstring(zt.read('Contents/section0.xml'))
root_tag = re.match(r'(<hs:sec [^>]*>)', zt.read('Contents/section0.xml').decode('utf-8').split('?>', 1)[1].lstrip()).group(1)
section_bytes = ("<?xml version='1.0' encoding='UTF-8' standalone='yes'?>\n" + root_tag + ''.join(parts) + '</hs:sec>').encode('utf-8')
etree.fromstring(section_bytes)       # well-formed?

# ------------------------------------------------------------------ package
hpf = zt.read('Contents/content.hpf').decode('utf-8')
items = ''.join(f'<opf:item id="{bid}" href="BinData/{bid}.png" media-type="image/png" isEmbeded="1"/>' for bid, _, _ in bin_items)
hpf = re.sub(r'<opf:item id="section\d+"[^>]*/>', '', hpf)
hpf = hpf.replace('<opf:item id="section0"', '<opf:item id="section0"')
hpf = re.sub(r'(<opf:item id="header"[^>]*/>)', r'\1<opf:item id="section0" href="Contents/section0.xml" media-type="application/xml"/>', hpf)
hpf = re.sub(r'<opf:item id="[^"]*" href="BinData/[^"]*"[^>]*/>', '', hpf)
hpf = hpf.replace('</opf:manifest>', items + '</opf:manifest>')
hpf = re.sub(r'<opf:itemref idref="section\d+" linear="yes"/>', '', hpf)
hpf = hpf.replace('<opf:itemref idref="header" linear="yes"/>', '<opf:itemref idref="header" linear="yes"/><opf:itemref idref="section0" linear="yes"/>')
hpf = re.sub(r'<opf:title[^>]*>[^<]*</opf:title>', '<opf:title xml:space="preserve">' + os.environ.get('DOC_TITLE', 'RuSQL 통합 문서 v29') + '</opf:title>', hpf)
preview = os.environ.get('DOC_PREVIEW', 'AI/MCP 통합 관계형 데이터베이스 관리 시스템 (RuSQL)\r\n프로젝트 통합 문서 | v29 | 김규현, 빌궁 | 2026년\r\n').encode('utf-8')

os.makedirs(os.path.dirname(os.path.abspath(OUT)), exist_ok=True)
with zipfile.ZipFile(OUT, 'w') as zo:
    for info in zt.infolist():
        n = info.filename
        if n.startswith('Contents/section') or n.startswith('BinData/') or n.startswith('Preview/') or n == 'Contents/header.xml' or n == 'Contents/content.hpf':
            continue
        zi = zipfile.ZipInfo(n, date_time=info.date_time); zi.compress_type = info.compress_type; zi.external_attr = info.external_attr
        zo.writestr(zi, zt.read(n))
    for n, data in (('Contents/header.xml', header_bytes), ('Contents/section0.xml', section_bytes), ('Contents/content.hpf', hpf.encode('utf-8')), ('Preview/PrvText.txt', preview)):
        zi = zipfile.ZipInfo(n, date_time=(2026, 11, 11, 0, 0, 0)); zi.compress_type = zipfile.ZIP_DEFLATED
        zo.writestr(zi, data)
    for bid, _, data in bin_items:
        zi = zipfile.ZipInfo(f'BinData/{bid}.png', date_time=(2026, 11, 11, 0, 0, 0)); zi.compress_type = zipfile.ZIP_STORED
        zo.writestr(zi, data)
print(f'written {OUT}: {len(parts)} body blocks, {len(char_reg.items)} char shapes, {len(para_reg.items)} paragraph shapes, {len(bf_reg.items) + 2} border fills, {len(bin_items)} pictures, tables {tbl_uid[0]}')
