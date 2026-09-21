from pathlib import Path
from xml.sax.saxutils import escape
from zipfile import ZIP_DEFLATED, ZipFile


OUT = Path(__file__).resolve().parents[1] / "benchmark_results" / "bao_cao_danh_gia_he_thong_buong_ngu.docx"
W = "http://schemas.openxmlformats.org/wordprocessingml/2006/main"
R = "http://schemas.openxmlformats.org/officeDocument/2006/relationships"


def x(text):
    return escape(str(text))


def run(text, bold=False, color=None, size=None, italic=False):
    rpr = []
    if bold:
        rpr.append("<w:b/>")
    if italic:
        rpr.append("<w:i/>")
    if color:
        rpr.append(f'<w:color w:val="{color}"/>')
    if size:
        rpr.append(f'<w:sz w:val="{size}"/><w:szCs w:val="{size}"/>')
    props = f"<w:rPr>{''.join(rpr)}</w:rPr>" if rpr else ""
    return f"<w:r>{props}<w:t xml:space=\"preserve\">{x(text)}</w:t></w:r>"


def paragraph(text="", style=None, bold=False, color=None, size=None, align=None, before=0, after=120, italic=False):
    ppr = []
    if style:
        ppr.append(f'<w:pStyle w:val="{style}"/>')
    if align:
        ppr.append(f'<w:jc w:val="{align}"/>')
    ppr.append(f'<w:spacing w:before="{before}" w:after="{after}" w:line="264" w:lineRule="auto"/>')
    return f"<w:p><w:pPr>{''.join(ppr)}</w:pPr>{run(text, bold, color, size, italic)}</w:p>"


def bullet(text):
    ppr = ('<w:pPr><w:pStyle w:val="Normal"/><w:numPr><w:ilvl w:val="0"/>'
           '<w:numId w:val="1"/></w:numPr><w:spacing w:after="80" w:line="264" '
           'w:lineRule="auto"/></w:pPr>')
    return f"<w:p>{ppr}{run(text)}</w:p>"


def page_break():
    return '<w:p><w:r><w:br w:type="page"/></w:r></w:p>'


def cell(text, width, header=False, align="left"):
    fill = '<w:shd w:fill="E8EEF5"/>' if header else ''
    p = (f'<w:p><w:pPr><w:jc w:val="{align}"/><w:spacing w:after="0"/></w:pPr>'
         f'{run(text, bold=header, color="1F4D78" if header else None, size=20 if header else 19)}</w:p>')
    return (f'<w:tc><w:tcPr><w:tcW w:w="{width}" w:type="dxa"/>{fill}'
            '<w:tcMar><w:top w:w="90" w:type="dxa"/><w:start w:w="120" w:type="dxa"/>'
            '<w:bottom w:w="90" w:type="dxa"/><w:end w:w="120" w:type="dxa"/></w:tcMar>'
            '<w:vAlign w:val="center"/></w:tcPr>' + p + '</w:tc>')


def table(headers, rows, widths):
    grid = ''.join(f'<w:gridCol w:w="{v}"/>' for v in widths)
    trs = ['<w:tr>' + ''.join(cell(h, widths[i], True, "center") for i, h in enumerate(headers)) + '</w:tr>']
    for row in rows:
        trs.append('<w:tr>' + ''.join(cell(v, widths[i], False, "center" if i > 0 else "left") for i, v in enumerate(row)) + '</w:tr>')
    borders = ''.join(f'<w:{side} w:val="single" w:sz="6" w:space="0" w:color="C9D3DF"/>' for side in ['top','left','bottom','right','insideH','insideV'])
    return (f'<w:tbl><w:tblPr><w:tblW w:w="9360" w:type="dxa"/><w:tblLayout w:type="fixed"/>'
            f'<w:tblBorders>{borders}</w:tblBorders><w:tblCellMar><w:top w:w="90" w:type="dxa"/>'
            '<w:start w:w="120" w:type="dxa"/><w:bottom w:w="90" w:type="dxa"/>'
            '<w:end w:w="120" w:type="dxa"/></w:tblCellMar></w:tblPr><w:tblGrid>' + grid + '</w:tblGrid>' + ''.join(trs) + '</w:tbl>')


def make_document():
    body = []
    body.append(paragraph("BAO CAO DANH GIA THU NGHIEM", bold=True, color="0B2545", size=34, align="center", before=500, after=60))
    body.append(paragraph("He thong phat hien buon ngu va mat tap trung cua tai xe tren ESP32-S3", color="2E74B5", size=24, align="center", after=300))
    body.append(paragraph("Phien benchmark: video dien thoai va log thiet bi ngay 09/09/2026", align="center", italic=True, color="555555", size=20, after=400))
    body.append(paragraph("Tom tat dieu hanh", style="Heading1", color="2E74B5", size=28, before=120, after=100))
    body.append(paragraph("Thu nghiem ghi nhan he thong van hanh on dinh trong cua so video 91.7 giay: landmark khuon mat hop le o 99.4% mau, khong ghi nhan loi watchdog hoac loi stream. Phat hien ngam va mat tap trung cho ket qua tot; phat hien nham mat va canh bao ngu gat can duoc cai tien vi nhieu EAR khi deo kinh hoac quay dau.", after=180))
    body.append(table(["Chi so chinh", "Gia tri"], [
        ["Thoi luong video danh gia", "91.7 giay"],
        ["So mau AI trong cua so video", "335 mau"],
        ["Ty le face + landmark hop le", "99.4%"],
        ["Cau hinh phan cung", "ESP32-S3 N16R8 + OV2640"],
        ["Nguon doi chieu", "Video dien thoai + log BENCH"],
    ], [4700, 4660]))
    body.append(paragraph("Ket luan nhanh: can giu mo hinh ngam va mat tap trung, dong thoi sua logic ngu gat theo huong loc trang thai quay dau va bo dem co hysteresis thoi gian.", bold=True, color="1F4D78", before=180, after=0))

    body.append(page_break())
    body.append(paragraph("1. Muc tieu va phuong phap danh gia", style="Heading1", color="2E74B5", size=28, before=0, after=100))
    body.append(paragraph("Muc tieu la do muc do dung cua ba hanh vi can canh bao: nham mat/ngu gat, ngam va mat tap trung. Danh gia su dung video quay bang dien thoai lam ground truth va cac dong BENCH,SAMPLE tu ESP32 lam du lieu du doan.", after=120))
    body.append(paragraph("Quy trinh", style="Heading2", color="2E74B5", size=24, before=120, after=70))
    for txt in [
        "Dong bo moc thoi gian video voi uptime cua ESP32 dua tren chuoi hanh vi ngam va cac dong BENCH,MARK.",
        "Gan nhan thu cong 5 lan ngam, 2 doan nham mat va 2 doan quay dau/mat tap trung tren video.",
        "So sanh tung mau AI voi khoang nhan tuong ung de tinh TP, FP, FN, TN; tu do tinh precision, recall va F1.",
        "Tinh do tre tu luc nhan video bat dau den mau du doan dau tien. Bao cao dong thoi canh bao state de phan biet tin hieu tho va canh bao thuc te.",
    ]:
        body.append(bullet(txt))
    body.append(paragraph("Phan biet chi so", style="Heading2", color="2E74B5", size=24, before=120, after=70))
    body.append(table(["Chi so", "Y nghia trong bao cao"], [
        ["Precision", "Ty le du doan duong tinh la dung; cao giup giam canh bao nham."],
        ["Recall", "Ty le hanh vi that su duoc phat hien; cao giup tranh bo sot."],
        ["F1", "Trung binh dieu hoa giua precision va recall."],
        ["Event recall", "Moi su kien duoc bat it nhat mot lan trong khoang nhan."],
        ["Latency", "Thoi gian tu khi hanh vi bat dau den khi he thong nhan ra."],
    ], [2450, 6910]))
    body.append(paragraph("Luu y: video quay bang dien thoai va gan nhan thu cong co sai so dong bo khoang +/- 0.5 giay. Day la benchmark ky thuat noi bo, chua phai chung nhan an toan hay danh gia lam sang.", italic=True, color="555555", before=140, after=0))

    body.append(page_break())
    body.append(paragraph("2. Ket qua dinh luong", style="Heading1", color="2E74B5", size=28, before=0, after=100))
    body.append(table(["Hanh vi", "Precision", "Recall", "F1", "Event recall", "Do tre p50 / p95"], [
        ["Nham mat (tin hieu tho)", "68.3%", "63.1%", "65.6%", "2/2", "627 / 762 ms"],
        ["Ngam", "100.0%", "72.8%", "84.3%", "5/5", "649 / 1012 ms"],
        ["Mat tap trung", "97.4%", "90.5%", "93.8%", "2/2", "642 / 1062 ms"],
    ], [2400, 1200, 1200, 900, 1350, 2310]))
    body.append(paragraph("Dien giai ket qua", style="Heading2", color="2E74B5", size=24, before=140, after=70))
    body.append(bullet("Ngam: phat hien day du 5/5 su kien, khong co duong tinh gia trong phien thu. Precision 100% la ket qua rat tot; recall 72.8% cho thay mot so mau trong doan ngam khong giu tin hieu lien tuc."))
    body.append(bullet("Mat tap trung: tin hieu raw co F1 93.8%. Canh bao DISTRACTED quan sat duoc sau 2.45-3.33 giay, phu hop voi chinh sach giu trang thai 2 giay de tranh bao dong do chuyen dong ngan."))
    body.append(bullet("Nham mat: ket qua raw thap hon ro ret. Do tre raw gan 0.63 giay khong dai dien cho canh bao ngu gat vi tin hieu bi ngat quang khi EAR dao dong."))
    body.append(paragraph("Canh bao MICROSLEEP", style="Heading2", color="2E74B5", size=24, before=120, after=70))
    body.append(paragraph("Quan sat video cho thay mot false positive MICROSLEEP khi tai xe quay dau, mat van mo. O doan nham mat lau, canh bao MICROSLEEP den muon xap xi 8 giay sau luc nham mat. Day la rui ro cao nhat cua phien benchmark va khong the dung chi so latency raw de ket luan toc do canh bao ngu gat.", after=0))

    body.append(page_break())
    body.append(paragraph("3. Phan tich nguyen nhan", style="Heading1", color="2E74B5", size=28, before=0, after=100))
    body.append(paragraph("Hien tuong chinh", style="Heading2", color="2E74B5", size=24, before=100, after=70))
    body.append(bullet("Khi quay dau, landmark mat va gia tri EAR kem on dinh; he thong co the hieu nham mat la nham sau."))
    body.append(bullet("Bo dem nham mat sau bi reset khi chi mot mau EAR vuot nguong. FPS AI thap hon camera, do do mot frame nhieu co the lam mat nhieu tram mili giay tien do canh bao."))
    body.append(bullet("State machine hien de MICROSLEEP uu tien hon DISTRACTED. Khi quay dau, nhieu EAR co the chuyen trang thai sang ngu gat thay vi duy tri mat tap trung."))
    body.append(paragraph("Y nghia cua false alarm rate", style="Heading2", color="2E74B5", size=24, before=120, after=70))
    body.append(paragraph("Ket qua ngoai suy false alarm theo gio trong phien 91.7 giay chua co y nghia thong ke. De danh gia ty le bao nham, can quay toi thieu 15-30 phut nguoi lai xe tinh tao, khuon mat nam trong khung hinh va moi truong anh sang dai dien. Chi so do tre va event recall trong phien ngan van huu ich de tim loi logic va so sanh truoc/sau khi sua.", after=0))
    body.append(paragraph("4. Ke hoach cai tien va tieu chi nghiem thu", style="Heading1", color="2E74B5", size=28, before=160, after=90))
    body.append(table(["Uu tien", "Thay doi ky thuat", "Tieu chi dat"], [
        ["P1", "Khoa danh gia nham mat khi attention_off/yaw lech; uu tien DISTRACTED khi dau quay.", "Khong co MICROSLEEP khi quay dau va mat mo."],
        ["P1", "Them hysteresis EAR va gap grace 400-600 ms; dung trung binh/median ngan de khang nhieu.", "Canh bao ngu gat ben vung, khong reset bo dem boi 1 frame."],
        ["P2", "Do benchmark theo state MICROSLEEP/DISTRACTED, ngoai tin hieu raw.", "Bao cao duoc do tre canh bao thuc te."],
        ["P2", "Thu 15-30 phut tinh tao va lap lai 10 lan moi hanh vi.", "Du lieu du de danh gia false alarm va do lap lai."],
    ], [900, 4950, 3510]))
    body.append(paragraph("Ket luan", style="Heading2", color="2E74B5", size=24, before=140, after=70))
    body.append(paragraph("He thong dat ket qua kha tot cho ngam va mat tap trung, dong thoi pipeline camera-landmark on dinh trong phien thu. Cong viec can thiet tiep theo la sua logic thoi gian cua nham mat/ngat EAR va uu tien dung ngu canh quay dau. Sau khi sua, lap lai cung kich ban video va them bai test tinh tao dai hon de co so lieu so sanh cong bang.", after=120))
    body.append(paragraph("Phu luc - Du lieu nguon: video 2871307567530747132.mp4, log BENCH da cung cap va file nhan benchmark_results/2026-09-09_phone_ground_truth.csv.", italic=True, color="555555", size=18, after=0))
    sect = ('<w:sectPr><w:pgSz w:w="12240" w:h="15840"/><w:pgMar w:top="1440" w:right="1440" w:bottom="1440" w:left="1440" w:header="708" w:footer="708" w:gutter="0"/>'
            '<w:footerReference w:type="default" r:id="rId2"/></w:sectPr>')
    return ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
            f'<w:document xmlns:w="{W}" xmlns:r="{R}"><w:body>{"".join(body)}{sect}</w:body></w:document>')


def main():
    OUT.parent.mkdir(parents=True, exist_ok=True)
    styles = f'''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:styles xmlns:w="{W}">
 <w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:ascii="Calibri" w:hAnsi="Calibri" w:eastAsia="Calibri" w:cs="Calibri"/><w:sz w:val="22"/><w:szCs w:val="22"/></w:rPr></w:rPrDefault></w:docDefaults>
 <w:style w:type="paragraph" w:default="1" w:styleId="Normal"><w:name w:val="Normal"/><w:qFormat/></w:style>
 <w:style w:type="paragraph" w:styleId="Heading1"><w:name w:val="Heading 1"/><w:basedOn w:val="Normal"/><w:qFormat/></w:style>
 <w:style w:type="paragraph" w:styleId="Heading2"><w:name w:val="Heading 2"/><w:basedOn w:val="Normal"/><w:qFormat/></w:style>
</w:styles>'''
    numbering = f'''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:numbering xmlns:w="{W}"><w:abstractNum w:abstractNumId="0"><w:lvl w:ilvl="0"><w:start w:val="1"/><w:numFmt w:val="bullet"/><w:lvlText w:val="-"/><w:lvlJc w:val="left"/><w:pPr><w:tabs><w:tab w:val="num" w:pos="720"/></w:tabs><w:ind w:left="720" w:hanging="360"/></w:pPr></w:lvl></w:abstractNum><w:num w:numId="1"><w:abstractNumId w:val="0"/></w:num></w:numbering>'''
    footer = f'''<?xml version="1.0" encoding="UTF-8" standalone="yes"?><w:ftr xmlns:w="{W}"><w:p><w:pPr><w:jc w:val="right"/></w:pPr><w:r><w:rPr><w:color w:val="777777"/><w:sz w:val="18"/></w:rPr><w:t>Benchmark ESP32-S3 | 09/09/2026</w:t></w:r></w:p></w:ftr>'''
    content_types = '''<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/><Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/><Override PartName="/word/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml"/><Override PartName="/word/numbering.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml"/><Override PartName="/word/footer1.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.footer+xml"/></Types>'''
    rels = '''<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/></Relationships>'''
    doc_rels = '''<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/><Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/footer" Target="footer1.xml"/><Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/numbering" Target="numbering.xml"/></Relationships>'''
    with ZipFile(OUT, "w", ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml", content_types)
        z.writestr("_rels/.rels", rels)
        z.writestr("word/document.xml", make_document())
        z.writestr("word/styles.xml", styles)
        z.writestr("word/numbering.xml", numbering)
        z.writestr("word/footer1.xml", footer)
        z.writestr("word/_rels/document.xml.rels", doc_rels)
    print(OUT)


if __name__ == "__main__":
    main()
