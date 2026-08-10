        let parsedElf = null;

        // Init drop-zone
        const dropZone = document.getElementById('drop-zone');
        const fileInput = document.getElementById('file-input');

        dropZone.addEventListener('click', () => fileInput.click());

        dropZone.addEventListener('dragover', (e) => {
            e.preventDefault();
            dropZone.classList.add('dragover');
        });

        dropZone.addEventListener('dragleave', () => {
            dropZone.classList.remove('dragover');
        });

        dropZone.addEventListener('drop', (e) => {
            e.preventDefault();
            dropZone.classList.remove('dragover');
            const files = e.dataTransfer.files;
            if (files.length > 0) {
                handleUploadedFile(files[0]);
            }
        });

        fileInput.addEventListener('change', () => {
            if (fileInput.files.length > 0) {
                handleUploadedFile(fileInput.files[0]);
            }
        });

        // Load JSON Button Click Handler
        document.getElementById('load-json-btn').addEventListener('click', () => {
            const pasteData = document.getElementById('json-paste').value.trim();
            if (!pasteData) {
                alert("Please paste the JSON export first.");
                return;
            }
            try {
                const jsonObj = JSON.parse(pasteData);
                if (!jsonObj.header) {
                    alert("Invalid JSON format. Make sure it contains 'header' definitions.");
                    return;
                }
                loadParsedElf(jsonObj, "JSON Paste Export");
            } catch (err) {
                alert("Failed to parse JSON: " + err.message);
            }
        });

        // Handle raw binary file upload
        function handleUploadedFile(file) {
            const loader = document.getElementById('loader');
            loader.classList.add('active');

            const reader = new FileReader();
            reader.onload = function(e) {
                try {
                    const arrayBuffer = e.target.result;
                    const parsed = parseElf32(arrayBuffer);
                    loadParsedElf(parsed, file.name);
                } catch (err) {
                    alert("Error parsing ELF: " + err.message);
                } finally {
                    loader.classList.remove('active');
                }
            };
            reader.onerror = function() {
                alert("Failed to read file.");
                loader.classList.remove('active');
            };
            reader.readAsArrayBuffer(file);
        }

        // JS ELF32 Parser Implementation
        function parseElf32(arrayBuffer) {
            const view = new DataView(arrayBuffer);
            // Verify Magic
            if (view.getUint32(0, false) !== 0x7F454C46) {
                throw new Error("Not a valid ELF binary file (Magic mismatch).");
            }
            
            const elfClass = view.getUint8(4); // 1 = 32-bit, 2 = 64-bit
            const elfData = view.getUint8(5); // 1 = Little-endian, 2 = Big-endian
            const isLittle = elfData === 1;

            if (elfClass !== 1) {
                throw new Error("This visualizer only supports 32-bit ELF files (ARM32 targets).");
            }

            const machine = view.getUint16(18, isLittle);
            if (machine !== 40) {
                throw new Error("Architecture Machine type mismatch: Expected EM_ARM (40). Got: " + machine);
            }

            const type = view.getUint16(16, isLittle);
            const entry = view.getUint32(24, isLittle);
            const phoff = view.getUint32(28, isLittle);
            const shoff = view.getUint32(32, isLittle);
            const flags = view.getUint32(36, isLittle);
            const phnum = view.getUint16(44, isLittle);
            const shnum = view.getUint16(48, isLittle);
            const shstrndx = view.getUint16(50, isLittle);

            // Program Headers
            const programHeaders = [];
            for (let i = 0; i < phnum; i++) {
                const off = phoff + i * 32;
                programHeaders.push({
                    type: view.getUint32(off, isLittle),
                    offset: view.getUint32(off + 4, isLittle),
                    vaddr: view.getUint32(off + 8, isLittle),
                    paddr: view.getUint32(off + 12, isLittle),
                    filesz: view.getUint32(off + 16, isLittle),
                    memsz: view.getUint32(off + 20, isLittle),
                    flags: view.getUint32(off + 24, isLittle),
                    align: view.getUint32(off + 28, isLittle)
                });
            }

            // Section Headers
            const sectionHeaders = [];
            for (let i = 0; i < shnum; i++) {
                const off = shoff + i * 40;
                sectionHeaders.push({
                    index: i,
                    nameOffset: view.getUint32(off, isLittle),
                    type: view.getUint32(off + 4, isLittle),
                    flags: view.getUint32(off + 8, isLittle),
                    addr: view.getUint32(off + 12, isLittle),
                    offset: view.getUint32(off + 16, isLittle),
                    size: view.getUint32(off + 20, isLittle),
                    link: view.getUint32(off + 24, isLittle),
                    info: view.getUint32(off + 28, isLittle),
                    addralign: view.getUint32(off + 32, isLittle),
                    entsize: view.getUint32(off + 36, isLittle)
                });
            }

            // String table mapping
            let shstrtab = new Uint8Array(0);
            if (shstrndx < shnum) {
                const s = sectionHeaders[shstrndx];
                shstrtab = new Uint8Array(arrayBuffer, s.offset, s.size);
            }

            const getStr = (buf, offset) => {
                let end = offset;
                while (end < buf.length && buf[end] !== 0) end++;
                return new TextDecoder().decode(buf.slice(offset, end));
            };

            sectionHeaders.forEach(sh => {
                sh.name = getStr(shstrtab, sh.nameOffset);
            });

            // Extract section raw bytes to hex strings
            const sectionData = {};
            sectionHeaders.forEach(sh => {
                if (sh.size > 0 && sh.type !== 8 /* SHT_NOBITS */) {
                    const length = Math.min(sh.size, arrayBuffer.byteLength - sh.offset);
                    if (length > 0) {
                        const arr = new Uint8Array(arrayBuffer, sh.offset, length);
                        let hex = "";
                        for (let j = 0; j < arr.length; j++) {
                            hex += arr[j].toString(16).padStart(2, '0');
                        }
                        sectionData[sh.name] = hex;
                    }
                }
            });

            // Symbols
            const symbols = [];
            sectionHeaders.forEach(sh => {
                if (sh.type === 2 /* SHT_SYMTAB */ || sh.type === 11 /* SHT_DYNSYM */) {
                    const count = sh.size / 16;
                    const strSec = sectionHeaders[sh.link];
                    const strData = new Uint8Array(arrayBuffer, strSec.offset, strSec.size);
                    for (let i = 0; i < count; i++) {
                        const off = sh.offset + i * 16;
                        const nameOffset = view.getUint32(off, isLittle);
                        const value = view.getUint32(off + 4, isLittle);
                        const size = view.getUint32(off + 8, isLittle);
                        const info = view.getUint8(off + 12);
                        const other = view.getUint8(off + 13);
                        const shndx = view.getUint16(off + 14, isLittle);
                        symbols.push({
                            name: getStr(strData, nameOffset),
                            value,
                            size,
                            bind: info >> 4,
                            type: info & 0xf,
                            shndx,
                            source: sh.name
                        });
                    }
                }
            });

            // Relocations
            const relocations = [];
            sectionHeaders.forEach(sh => {
                if (sh.type === 9 /* SHT_REL */ || sh.type === 4 /* SHT_RELA */) {
                    const hasAddend = sh.type === 4;
                    const entSize = sh.entsize || (hasAddend ? 12 : 8);
                    const count = sh.size / entSize;
                    for (let i = 0; i < count; i++) {
                        const off = sh.offset + i * entSize;
                        const r_offset = view.getUint32(off, isLittle);
                        const r_info = view.getUint32(off + 4, isLittle);
                        const r_addend = hasAddend ? view.getInt32(off + 8, isLittle) : 0;
                        relocations.push({
                            section: sh.name,
                            offset: r_offset,
                            info: r_info,
                            symIdx: r_info >> 8,
                            type: r_info & 0xff,
                            addend: r_addend,
                            hasAddend
                        });
                    }
                }
            });

            // Dynamic tags
            const dynamic = [];
            sectionHeaders.forEach(sh => {
                if (sh.type === 6 /* SHT_DYNAMIC */) {
                    const count = sh.size / 8;
                    const strSec = sectionHeaders[sh.link];
                    const strData = strSec ? new Uint8Array(arrayBuffer, strSec.offset, strSec.size) : null;
                    for (let i = 0; i < count; i++) {
                        const off = sh.offset + i * 8;
                        const tag = view.getInt32(off, isLittle);
                        const val = view.getUint32(off + 4, isLittle);
                        let str = "";
                        if (strData && (tag === 1 || tag === 14 || tag === 15 || tag === 23)) {
                            str = getStr(strData, val);
                        }
                        dynamic.push({
                            tag,
                            val,
                            tag_name: getDynTagName(tag),
                            str
                        });
                    }
                }
            });

            // PLT Map
            const plt = [];
            const pltSec = sectionHeaders.find(s => s.name === ".plt");
            if (pltSec) {
                const entries = Math.floor(pltSec.size / 16);
                const rels = relocations.filter(r => r.section === ".rel.plt" || r.section === ".rela.plt");
                
                let dynsyms = [];
                const relPltSec = sectionHeaders.find(s => s.name === ".rel.plt" || s.name === ".rela.plt");
                if (relPltSec) {
                    const dynsymSec = sectionHeaders[relPltSec.link];
                    const dynstrSec = sectionHeaders[dynsymSec.link];
                    const dynstrData = new Uint8Array(arrayBuffer, dynstrSec.offset, dynstrSec.size);
                    const count = dynsymSec.size / 16;
                    for (let i = 0; i < count; i++) {
                        const off = dynsymSec.offset + i * 16;
                        const nameOffset = view.getUint32(off, isLittle);
                        dynsyms.push(getStr(dynstrData, nameOffset));
                    }
                }

                for (let i = 0; i < entries; i++) {
                    let symbol = "";
                    if (i > 0 && rels[i - 1]) {
                        const r = rels[i - 1];
                        symbol = dynsyms[r.symIdx] || "";
                    }
                    plt.push({
                        index: i,
                        addr: pltSec.addr + i * 16,
                        symbol
                    });
                }
            }

            return {
                header: { type, machine, entry, phoff, shoff, phnum, shnum },
                program_headers: programHeaders,
                section_headers: sectionHeaders,
                symbols,
                relocations,
                dynamic,
                plt,
                section_data: sectionData
            };
        }

        function getDynTagName(tag) {
            const tags = {
                0: "DT_NULL", 1: "DT_NEEDED", 2: "DT_PLTRELSZ", 3: "DT_PLTGOT", 4: "DT_HASH",
                5: "DT_STRTAB", 6: "DT_SYMTAB", 7: "DT_RELA", 8: "DT_RELASZ", 9: "DT_RELAENT",
                10: "DT_STRSZ", 11: "DT_SYMENT", 14: "DT_SONAME", 15: "DT_RPATH", 17: "DT_JMPREL",
                19: "DT_INIT_ARRAY", 20: "DT_FINI_ARRAY", 21: "DT_INIT_ARRAYSZ", 22: "DT_FINI_ARRAYSZ",
                23: "DT_RUNPATH", 24: "DT_FLAGS", 27: "DT_REL", 28: "DT_RELSZ", 29: "DT_RELENT",
                30: "DT_PLTREL", 32: "DT_DEBUG", 33: "DT_TEXTREL", 34: "DT_JMPREL", 35: "DT_BIND_NOW",
                36: "DT_INIT", 37: "DT_FINI"
            };
            return tags[tag] || "DT_???";
        }

        // Load Parsed ELF object into UI
        function loadParsedElf(elfObj, filename) {
            parsedElf = elfObj;
            
            // Set file status
            const statusBadge = document.getElementById('file-status');
            statusBadge.style.display = 'inline-flex';
            statusBadge.innerText = filename;

            // Show dashboard and hide empty state
            document.getElementById('empty-state').style.display = 'none';
            document.getElementById('dashboard-content').style.display = 'block';
            document.getElementById('header-card').style.display = 'block';

            // Fill header metadata
            const metadataList = document.getElementById('header-meta-list');
            metadataList.innerHTML = `
                <li><span>ELF Class</span><span>32-bit</span></li>
                <li><span>Endianness</span><span>Little-endian</span></li>
                <li><span>Target CPU</span><span>ARM (EM_ARM)</span></li>
                <li><span>Entry Point</span><span>0x${parsedElf.header.entry.toString(16).padStart(8, '0')}</span></li>
                <li><span>Type</span><span>${parsedElf.header.type === 2 ? 'Executable' : parsedElf.header.type === 1 ? 'Relocatable' : 'Shared (Dynamic)'}</span></li>
                <li><span>Program Segments</span><span>${parsedElf.header.phnum}</span></li>
                <li><span>Sections count</span><span>${parsedElf.header.shnum}</span></li>
            `;

            // Setup overview tables
            populateOverview();

            // Populate all tabs
            populateSegments();
            populateSections();
            populateSymbols();
            populateDynamic();

            // Reset trace query input to entry point
            document.getElementById('trace-query-input').value = `0x${parsedElf.header.entry.toString(16)}`;
            
            // Default to overview tab
            switchTab('overview');
        }

        function switchTab(tabId) {
            const tabs = document.querySelectorAll('.tab-content');
            tabs.forEach(tab => tab.classList.remove('active'));
            
            const buttons = document.querySelectorAll('.tab-btn');
            buttons.forEach(btn => btn.classList.remove('active'));

            document.getElementById(`tab-${tabId}`).classList.add('active');
            
            // Highlight button
            const btn = Array.from(buttons).find(b => b.innerText.toLowerCase().includes(tabId.substring(0, 4)));
            if (btn) btn.classList.add('active');

            if (tabId === 'trace') {
                runTrace();
            }
        }

        // POPULATE OVERVIEW TAB
        function populateOverview() {
            // Memory layout visualizer
            const mapContainer = document.getElementById('memory-map-visual');
            mapContainer.innerHTML = '';

            // Render program loadable segments
            const loadSegments = parsedElf.program_headers.filter(ph => ph.type === 1 /* PT_LOAD */);
            
            if (loadSegments.length === 0) {
                mapContainer.innerHTML = `<div style="text-align:center; padding: 20px; color: var(--text-secondary);">No LOAD Segments detected. Typical for relocatable object files.</div>`;
            } else {
                loadSegments.forEach((seg, sIdx) => {
                    const wrapper = document.createElement('div');
                    wrapper.className = 'segment-bar-wrapper';

                    const info = document.createElement('div');
                    info.className = 'segment-info';
                    info.innerHTML = `
                        <span class="seg-title">LOAD Segment [${sIdx}]</span>
                        <span>Virt Addr range: 0x${seg.vaddr.toString(16)} - 0x${(seg.vaddr + seg.memsz).toString(16)} | File offset: 0x${seg.offset.toString(16)}</span>
                    `;
                    wrapper.appendChild(info);

                    const bar = document.createElement('div');
                    bar.className = 'segment-bar';

                    // Find sections nested in this segment
                    const nestedSections = parsedElf.section_headers.filter(sh => 
                        sh.addr >= seg.vaddr && sh.addr < (seg.vaddr + seg.memsz) && sh.size > 0
                    );

                    nestedSections.forEach(sh => {
                        const secBlock = document.createElement('div');
                        secBlock.className = 'sec-block';
                        const pct = (sh.size / seg.memsz) * 100;
                        secBlock.style.width = `${Math.max(10, pct)}%`; // ensure it's visible
                        secBlock.innerText = sh.name;
                        secBlock.title = `${sh.name} (Addr: 0x${sh.addr.toString(16)}, Size: ${sh.size} bytes)`;
                        secBlock.onclick = () => {
                            document.getElementById('trace-query-input').value = sh.name;
                            switchTab('trace');
                        };
                        bar.appendChild(secBlock);
                    });

                    wrapper.appendChild(bar);
                    mapContainer.appendChild(wrapper);
                });
            }

            // Overview Tables
            const secTable = document.getElementById('overview-sections-table');
            secTable.innerHTML = '';
            const keySections = ['.text', '.data', '.bss', '.rodata', '.plt', '.got'];
            parsedElf.section_headers.forEach(sh => {
                if (keySections.includes(sh.name)) {
                    const tr = document.createElement('tr');
                    tr.innerHTML = `
                        <td style="font-weight: 500;">${sh.name}</td>
                        <td class="mono-cell">0x${sh.addr.toString(16)}</td>
                        <td class="mono-cell">0x${sh.offset.toString(16)}</td>
                        <td>${sh.size} B</td>
                    `;
                    secTable.appendChild(tr);
                }
            });

            const symTable = document.getElementById('overview-symbols-table');
            symTable.innerHTML = '';
            const importantSyms = ['_start', 'main', 'Reset_Handler', 'global_var', 'static_var', 'add_numbers'];
            parsedElf.symbols.forEach(s => {
                if (importantSyms.includes(s.name) || (s.name && s.bind === 1 && s.type === 2 && symTable.children.length < 8)) {
                    const tr = document.createElement('tr');
                    tr.innerHTML = `
                        <td style="font-weight: 500;">${s.name}</td>
                        <td class="mono-cell">0x${s.value.toString(16)}</td>
                        <td><span class="flag-badge ${s.type === 2 ? 'ax' : 'wa'}">${s.type === 2 ? 'FUNC' : 'VAR'}</span></td>
                        <td><button onclick="inspectSymbol('${s.name}')" class="action-btn">Trace</button></td>
                    `;
                    symTable.appendChild(tr);
                }
            });
        }

        // SEGMENTS TAB POPULATION
        function populateSegments() {
            const body = document.getElementById('segments-table-body');
            body.innerHTML = '';
            parsedElf.program_headers.forEach((ph, idx) => {
                const tr = document.createElement('tr');
                const types = { 0: "NULL", 1: "LOAD", 2: "DYNAMIC", 3: "INTERP", 4: "NOTE", 5: "SHLIB", 6: "PHDR" };
                const typeStr = types[ph.type] || `PT_(${ph.type})`;

                let flagStr = "";
                if (ph.flags & 4) flagStr += "R";
                if (ph.flags & 2) flagStr += "W";
                if (ph.flags & 1) flagStr += "X";

                tr.innerHTML = `
                    <td class="mono-cell">[${idx}]</td>
                    <td style="font-weight: 600; color: var(--accent-secondary);">${typeStr}</td>
                    <td><span class="flag-badge ${flagStr.includes('X') ? 'ax' : (flagStr.includes('W') ? 'wa' : '')}">${flagStr}</span></td>
                    <td class="mono-cell"><a href="#" onclick="inspectAddress(0x${ph.vaddr.toString(16)}); return false;" style="color: var(--accent-primary); text-decoration: none; border-bottom: 1px dashed var(--accent-primary);">0x${ph.vaddr.toString(16)}</a></td>
                    <td class="mono-cell">0x${ph.offset.toString(16)}</td>
                    <td>${ph.filesz} B</td>
                    <td>${ph.memsz} B</td>
                    <td class="mono-cell">${ph.align}</td>
                `;
                body.appendChild(tr);
            });
        }

        // SECTIONS TAB POPULATION
        function populateSections() {
            const body = document.getElementById('sections-table-body');
            body.innerHTML = '';
            parsedElf.section_headers.forEach((sh, idx) => {
                const tr = document.createElement('tr');
                tr.id = `sec-row-${idx}`;
                
                const typeNames = { 0: "NULL", 1: "PROGBITS", 2: "SYMTAB", 3: "STRTAB", 4: "RELA", 5: "HASH", 6: "DYNAMIC", 7: "NOTE", 8: "NOBITS", 9: "REL", 10: "SHLIB", 11: "DYNSYM" };
                const typeStr = typeNames[sh.type] || `SHT_(${sh.type})`;

                let flagStr = "";
                if (sh.flags & 1) flagStr += "W";
                if (sh.flags & 2) flagStr += "A";
                if (sh.flags & 4) flagStr += "X";

                tr.innerHTML = `
                    <td class="mono-cell">${idx}</td>
                    <td style="font-weight: 600; color: var(--text-main);"><a href="#" onclick="inspectSection('${sh.name}'); return false;" style="color: var(--text-main); text-decoration: none; border-bottom: 1px dashed rgba(255,255,255,0.3);">${sh.name}</a></td>
                    <td>${typeStr}</td>
                    <td><span class="flag-badge ${flagStr.includes('X') ? 'ax' : (flagStr.includes('W') ? 'wa' : '')}">${flagStr}</span></td>
                    <td class="mono-cell"><a href="#" onclick="inspectAddress(0x${sh.addr.toString(16)}); return false;" style="color: var(--accent-secondary); text-decoration: none; border-bottom: 1px dashed var(--accent-secondary);">0x${sh.addr.toString(16)}</a></td>
                    <td class="mono-cell">0x${sh.offset.toString(16)}</td>
                    <td>${sh.size} B</td>
                    <td class="mono-cell">${sh.entsize}</td>
                    <td><button onclick="inspectSection('${sh.name}')" class="action-btn">View Hex</button></td>
                `;
                body.appendChild(tr);
            });
        }

        function filterSections() {
            const search = document.getElementById('section-search').value.toLowerCase();
            const typeFilter = document.getElementById('section-type-filter').value;
            
            parsedElf.section_headers.forEach((sh, idx) => {
                const row = document.getElementById(`sec-row-${idx}`);
                const matchesSearch = sh.name.toLowerCase().includes(search);
                const matchesType = typeFilter === 'all' || sh.type.toString() === typeFilter;

                if (matchesSearch && matchesType) {
                    row.style.display = '';
                } else {
                    row.style.display = 'none';
                }
            });
        }

        // SYMBOLS TAB POPULATION
        function populateSymbols() {
            const body = document.getElementById('symbols-table-body');
            body.innerHTML = '';
            parsedElf.symbols.forEach((s, idx) => {
                const tr = document.createElement('tr');
                tr.id = `sym-row-${idx}`;

                const binds = { 0: "LOCAL", 1: "GLOBAL", 2: "WEAK" };
                const types = { 0: "NOTYPE", 1: "OBJECT", 2: "FUNC", 3: "SECTION", 4: "FILE" };

                const bindStr = binds[s.bind] || `BIND(${s.bind})`;
                const typeStr = types[s.type] || `TYPE(${s.type})`;

                tr.innerHTML = `
                    <td style="font-weight: 500; max-width: 250px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap;">${s.name || '<unnamed>'}</td>
                    <td class="mono-cell">0x${s.value.toString(16)}</td>
                    <td>${s.size} B</td>
                    <td><span class="flag-badge ${s.bind === 1 ? 'wa' : ''}">${bindStr}</span></td>
                    <td><span class="flag-badge ${s.type === 2 ? 'ax' : ''}">${typeStr}</span></td>
                    <td class="mono-cell">${s.shndx === 0 ? 'UNDEF' : (s.shndx === 65521 ? 'ABS' : `Sec ${s.shndx}`)}</td>
                    <td style="color: var(--text-secondary);">${s.source}</td>
                    <td><button onclick="inspectSymbol('${s.name}')" class="action-btn">Trace</button></td>
                `;
                body.appendChild(tr);
            });
        }

        function filterSymbols() {
            const search = document.getElementById('symbol-search').value.toLowerCase();
            const typeFilter = document.getElementById('symbol-type-filter').value;
            const bindFilter = document.getElementById('symbol-bind-filter').value;

            parsedElf.symbols.forEach((s, idx) => {
                const row = document.getElementById(`sym-row-${idx}`);
                const matchesSearch = s.name.toLowerCase().includes(search);
                const matchesType = typeFilter === 'all' || s.type.toString() === typeFilter;
                const matchesBind = bindFilter === 'all' || s.bind.toString() === bindFilter;

                if (matchesSearch && matchesType && matchesBind) {
                    row.style.display = '';
                } else {
                    row.style.display = 'none';
                }
            });
        }

        // DYNAMIC TAB POPULATION
        function populateDynamic() {
            const dynBody = document.getElementById('dynamic-table-body');
            dynBody.innerHTML = '';
            
            if (parsedElf.dynamic.length === 0) {
                dynBody.innerHTML = `<tr><td colspan="4" style="text-align:center; color: var(--text-secondary)">No Dynamic tags detected.</td></tr>`;
            } else {
                parsedElf.dynamic.forEach(d => {
                    const tr = document.createElement('tr');
                    const isAddr = (d.tag === 3 || d.tag === 4 || d.tag === 5 || d.tag === 6 || d.tag === 7 || d.tag === 27 || d.tag === 17 || d.tag === 36 || d.tag === 37);
                    
                    let valHtml = `0x${d.val.toString(16)}`;
                    if (isAddr) {
                        valHtml = `<a href="#" onclick="inspectAddress(0x${d.val.toString(16)}); return false;" style="color: var(--accent-orange); text-decoration: none; border-bottom: 1px dashed var(--accent-orange);">0x${d.val.toString(16)}</a>`;
                    }
                    
                    tr.innerHTML = `
                        <td class="mono-cell">${d.tag}</td>
                        <td style="font-weight: 500; color: var(--accent-orange);">${d.tag_name}</td>
                        <td class="mono-cell">${valHtml}</td>
                        <td>${d.str ? `-> <strong style="color: var(--accent-primary)">"${d.str}"</strong>` : ''}</td>
                    `;
                    dynBody.appendChild(tr);
                });
            }

            const relocBody = document.getElementById('relocs-table-body');
            relocBody.innerHTML = '';
            
            if (parsedElf.relocations.length === 0) {
                relocBody.innerHTML = `<tr><td colspan="5" style="text-align:center; color: var(--text-secondary)">No Relocation records.</td></tr>`;
            } else {
                parsedElf.relocations.forEach(r => {
                    const tr = document.createElement('tr');
                    
                    // Map symbol index if we have loaded symbol tables
                    let symName = "";
                    if (parsedElf.symbols.length > 0 && r.symIdx < parsedElf.symbols.length) {
                        symName = parsedElf.symbols[r.symIdx].name || "";
                    }

                    let symHtml = symName;
                    if (symName) {
                        symHtml = `<a href="#" onclick="inspectSymbol('${symName}'); return false;" style="color: var(--accent-green); text-decoration: none; border-bottom: 1px dashed var(--accent-green);">${symName}</a>`;
                    }

                    tr.innerHTML = `
                        <td class="mono-cell"><a href="#" onclick="inspectAddress(0x${r.offset.toString(16)}); return false;" style="color: var(--accent-secondary); text-decoration: none; border-bottom: 1px dashed var(--accent-secondary);">0x${r.offset.toString(16)}</a></td>
                        <td class="mono-cell">${r.type}</td>
                        <td style="font-weight: 600; color: var(--accent-secondary);">${getRelocName(r.type)}</td>
                        <td class="mono-cell">${r.symIdx}</td>
                        <td style="color: var(--accent-green); font-weight: 500;">${symHtml}</td>
                    `;
                    relocBody.appendChild(tr);
                });
            }

            // PLT Map
            const pltBody = document.getElementById('plt-table-body');
            pltBody.innerHTML = '';
            if (parsedElf.plt.length === 0) {
                pltBody.innerHTML = `<tr><td colspan="3" style="text-align:center; color: var(--text-secondary)">No PLT entries detected.</td></tr>`;
            } else {
                parsedElf.plt.forEach(p => {
                    const tr = document.createElement('tr');
                    
                    let symHtml = 'reserved (dynamic resolver)';
                    if (p.symbol) {
                        symHtml = `<a href="#" onclick="inspectSymbol('${p.symbol}'); return false;" style="color: var(--accent-primary); text-decoration: none; border-bottom: 1px dashed var(--accent-primary);">${p.symbol} ()</a>`;
                    }
                    
                    tr.innerHTML = `
                        <td class="mono-cell">entry[${p.index}]</td>
                        <td class="mono-cell"><a href="#" onclick="inspectAddress(0x${p.addr.toString(16)}); return false;" style="color: var(--accent-secondary); text-decoration: none; border-bottom: 1px dashed var(--accent-secondary);">0x${p.addr.toString(16)}</a></td>
                        <td style="color: var(--accent-primary); font-weight: 600;">${symHtml}</td>
                    `;
                    pltBody.appendChild(tr);
                });
            }
        }

        function getRelocName(type) {
            const names = {
                0: "R_ARM_NONE", 1: "R_ARM_PC24", 2: "R_ARM_ABS32", 3: "R_ARM_REL32",
                5: "R_ARM_ABS16", 6: "R_ARM_ABS12", 7: "R_ARM_THM_ABS5", 8: "R_ARM_ABS8",
                9: "R_ARM_SBREL32", 10: "R_ARM_THM_CALL", 11: "R_ARM_THM_PC8",
                21: "R_ARM_PLT32", 22: "R_ARM_CALL", 23: "R_ARM_JUMP24", 24: "R_ARM_THM_JUMP24",
                25: "R_ARM_BASE_ABS", 100: "R_ARM_JUMP_SLOT", 101: "R_ARM_RELATIVE",
                102: "R_ARM_GLOB_DAT", 103: "R_ARM_GOT32", 104: "R_ARM_PLT32_REL"
            };
            return names[type] || `R_ARM_(${type})`;
        }

        // TRACER NAVIGATION HISTORY
        const traceHistory = [];
        let historyIndex = -1;
        let isNavigatingHistory = false;

        function navigateHistory(direction) {
            if (direction === -1 && historyIndex > 0) {
                historyIndex--;
                isNavigatingHistory = true;
                document.getElementById('trace-query-input').value = traceHistory[historyIndex];
                runTrace();
                isNavigatingHistory = false;
                updateHistoryButtons();
            } else if (direction === 1 && historyIndex < traceHistory.length - 1) {
                historyIndex++;
                isNavigatingHistory = true;
                document.getElementById('trace-query-input').value = traceHistory[historyIndex];
                runTrace();
                isNavigatingHistory = false;
                updateHistoryButtons();
            }
        }

        function updateHistoryButtons() {
            const backBtn = document.getElementById('history-back-btn');
            const fwdBtn = document.getElementById('history-fwd-btn');
            if (backBtn && fwdBtn) {
                backBtn.disabled = historyIndex <= 0;
                fwdBtn.disabled = historyIndex >= traceHistory.length - 1;
                backBtn.style.opacity = backBtn.disabled ? 0.4 : 1;
                backBtn.style.cursor = backBtn.disabled ? 'not-allowed' : 'pointer';
                fwdBtn.style.opacity = fwdBtn.disabled ? 0.4 : 1;
                fwdBtn.style.cursor = fwdBtn.disabled ? 'not-allowed' : 'pointer';
            }
        }

        // TRACER ENGINE IMPLEMENTATION
        function inspectAddress(vaddr) {
            document.getElementById('trace-query-input').value = `0x${vaddr.toString(16)}`;
            switchTab('trace');
        }

        function inspectSymbol(name) {
            document.getElementById('trace-query-input').value = name;
            switchTab('trace');
        }

        function inspectSection(name) {
            document.getElementById('trace-query-input').value = name;
            switchTab('trace');
        }

        function runTrace() {
            const query = document.getElementById('trace-query-input').value.trim();
            if (!query) return;

            if (!isNavigatingHistory) {
                if (historyIndex < traceHistory.length - 1) {
                    traceHistory.splice(historyIndex + 1);
                }
                if (traceHistory.length === 0 || traceHistory[traceHistory.length - 1] !== query) {
                    traceHistory.push(query);
                    historyIndex = traceHistory.length - 1;
                }
                updateHistoryButtons();
            }

            let vaddr = 0;
            let source = "";
            let found = false;

            if (query === 'entry') {
                vaddr = parsedElf.header.entry;
                source = "ELF Header Entry Point";
                found = true;
            } else if (query.startsWith('0x') || query.startsWith('0X')) {
                vaddr = parseInt(query, 16);
                source = "Hex Address input";
                found = true;
            } else if (!isNaN(query)) {
                vaddr = parseInt(query, 10);
                source = "Decimal Address input";
                found = true;
            } else {
                // Search in symbol table
                const sym = parsedElf.symbols.find(s => s.name === query);
                if (sym) {
                    vaddr = sym.value;
                    source = `Symbol lookup: "${query}"`;
                    found = true;
                } else {
                    // Search in section headers by name
                    const sec = parsedElf.section_headers.find(s => s.name === query);
                    if (sec) {
                        vaddr = sec.addr;
                        source = `Section lookup: "${query}"`;
                        found = true;
                    }
                }
            }

            if (!found) {
                alert(`Could not resolve query "${query}" to a virtual address.`);
                return;
            }

            // Adjust ARM Thumb mode symbol address (LSB is odd)
            let isThumb = false;
            let originalVaddr = vaddr;
            if (vaddr & 1) {
                vaddr &= 0xFFFFFFFE;
                isThumb = true;
            }

            const traceViz = document.getElementById('trace-visualizer');
            traceViz.style.display = 'flex';
            traceViz.innerHTML = '';

            // Step 1: Input node
            addNode(traceViz, "IN", "Query Input", `${query} ${isThumb ? '(Thumb Mode resolved)' : ''}`, "blue");
            addArrow(traceViz);

            // Step 2: ELF Header
            addNode(traceViz, "ELF", "ELF Header Info", `Entry point: 0x${parsedElf.header.entry.toString(16)} | Sections: ${parsedElf.header.shnum}`, "blue");
            addArrow(traceViz);

            // Step 3: Segment Mapping (Program Header)
            const seg = parsedElf.program_headers.find(ph => 
                (ph.type === 1 /* PT_LOAD */ || ph.type === 2) && vaddr >= ph.vaddr && vaddr < (ph.vaddr + ph.memsz)
            );
            
            let fileOffset = -1;
            if (seg) {
                const offsetInSeg = vaddr - seg.vaddr;
                fileOffset = seg.offset + offsetInSeg;
                addNode(traceViz, "SEG", `Segment PT_LOAD`, `Virtual address range: 0x${seg.vaddr.toString(16)} - 0x${(seg.vaddr + seg.memsz).toString(16)} | File offset: 0x${seg.offset.toString(16)} | Offset in segment: 0x${offsetInSeg.toString(16)}`, "purple");
                addArrow(traceViz);
            } else {
                addNode(traceViz, "SEG", "Segment Map Warning", "Address does not reside inside any LOAD segment.", "red");
                addArrow(traceViz);
            }

            // Step 4: Section Mapping
            let sec = null;
            if (isNaN(query) && !query.startsWith('0x') && !query.startsWith('0X') && query !== 'entry') {
                sec = parsedElf.section_headers.find(sh => sh.name === query);
            }
            if (!sec) {
                sec = parsedElf.section_headers.find(sh => 
                    sh.addr !== 0 && vaddr >= sh.addr && vaddr < (sh.addr + sh.size)
                );
            }

            if (!seg && sec) {
                fileOffset = sec.offset + (vaddr - sec.addr);
            }

            let hexData = "";
            let secName = "N/A";
            let offsetInSec = 0;

            if (sec) {
                secName = sec.name;
                offsetInSec = vaddr - sec.addr;
                addNode(traceViz, "SEC", `Section "${sec.name}"`, `Start address: 0x${sec.addr.toString(16)} | Size: ${sec.size} bytes | Offset inside Section: 0x${offsetInSec.toString(16)} | Flags: 0x${sec.flags.toString(16)}`, "purple");
                addArrow(traceViz);
                
                // Get hex content from this section
                hexData = parsedElf.section_data[sec.name] || "";
            } else {
                addNode(traceViz, "SEC", "Section Map Warning", "No containing section header found for this address.", "red");
                addArrow(traceViz);
            }

            // Step 5: Effective file offset
            if (fileOffset !== -1) {
                addNode(traceViz, "OFF", "Resolved File Offset", `Resulting Offset in Binary File: 0x${fileOffset.toString(16)}`, "green");
            } else {
                addNode(traceViz, "OFF", "Unmapped Offset", "Address points to uninitialized virtual space (no file bytes).", "red");
            }

            // Show address in Hex indicator
            document.getElementById('hex-section-indicator').innerText = `Section: ${secName} (offset: 0x${offsetInSec.toString(16)})`;

            // RENDER HEX VIEW
            renderHexView(hexData, sec ? sec.addr : vaddr, offsetInSec);

            // RENDER DISASSEMBLY
            renderDisasmView(hexData, sec ? sec.addr : vaddr, offsetInSec, sec && (sec.flags & 4) /* SHF_EXECINSTR */, isThumb);
        }

        function addNode(container, tag, title, desc, colorClass) {
            const node = document.createElement('div');
            node.className = `flow-node ${colorClass} active`;
            node.innerHTML = `
                <div class="flow-icon">${tag}</div>
                <div class="flow-details">
                    <h4>${title}</h4>
                    <p>${desc}</p>
                </div>
            `;
            container.appendChild(node);
        }

        function addArrow(container) {
            const arrow = document.createElement('div');
            arrow.className = 'flow-arrow';
            arrow.innerHTML = '&#8595;';
            container.appendChild(arrow);
        }

        // HEX VIEWER COMPONENT
        function renderHexView(hexStr, startAddr, offset) {
            const container = document.getElementById('hex-view-container');
            container.innerHTML = '';

            if (!hexStr) {
                container.innerHTML = `<div style="color: var(--accent-red);">No data available to display for this address.</div>`;
                return;
            }

            const bytes = [];
            for (let i = 0; i < hexStr.length; i += 2) {
                bytes.push(hexStr.substring(i, i + 2));
            }

            // We display 128 bytes centered around the offset or starting at it
            const viewStart = Math.max(0, Math.floor(offset / 16) * 16 - 32);
            const viewEnd = Math.min(bytes.length, viewStart + 256);

            for (let addr = viewStart; addr < viewEnd; addr += 16) {
                const trAddr = startAddr + addr;
                const isHighlightRow = offset >= addr && offset < (addr + 16);

                const row = document.createElement('div');
                row.className = `hex-row ${isHighlightRow ? 'highlighted' : ''}`;

                // Address Column
                const addrCol = document.createElement('span');
                addrCol.className = 'hex-addr';
                addrCol.innerText = `0x${trAddr.toString(16).toUpperCase().padStart(8, '0')}`;
                row.appendChild(addrCol);

                // Hex Bytes Column
                const bytesCol = document.createElement('div');
                bytesCol.className = 'hex-bytes';
                
                let asciiStr = "";
                
                // Group bytes in 4-byte words to verify pointers
                for (let group = 0; group < 4; group++) {
                    const wordOffset = addr + group * 4;
                    let hasWord = false;
                    let wordVal = 0;
                    
                    if (wordOffset + 4 <= bytes.length) {
                        const b0 = parseInt(bytes[wordOffset], 16);
                        const b1 = parseInt(bytes[wordOffset+1], 16);
                        const b2 = parseInt(bytes[wordOffset+2], 16);
                        const b3 = parseInt(bytes[wordOffset+3], 16);
                        wordVal = b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
                        hasWord = true;
                    }

                    let refName = "";
                    if (hasWord && wordVal !== 0) {
                        const checkAddr = (wordVal & 1) ? (wordVal & ~1) : wordVal;
                        const sym = parsedElf.symbols.find(s => s.value === checkAddr || s.value === wordVal);
                        if (sym && sym.name) {
                            refName = `Symbol: ${sym.name}`;
                        } else {
                            const sec = parsedElf.section_headers.find(s => s.addr !== 0 && checkAddr >= s.addr && checkAddr < (s.addr + s.size));
                            if (sec) {
                                refName = `Section: ${sec.name} + 0x${(checkAddr - sec.addr).toString(16)}`;
                            }
                        }
                    }

                    const wordSpan = document.createElement('span');
                    wordSpan.className = 'hex-word-group';
                    if (refName) {
                        wordSpan.style.borderBottom = '1px dashed var(--accent-green)';
                        wordSpan.style.cursor = 'pointer';
                        wordSpan.title = `${refName} (0x${wordVal.toString(16).toUpperCase()}) - Click to jump/trace`;
                        wordSpan.onclick = () => inspectAddress(wordVal);
                    }

                    for (let j = 0; j < 4; j++) {
                        const bIdx = wordOffset + j;
                        const byteSpan = document.createElement('span');
                        
                        if (bIdx < bytes.length) {
                            const byteVal = bytes[bIdx];
                            byteSpan.innerText = byteVal.toUpperCase();
                            
                            if (bIdx === offset) {
                                byteSpan.style.color = 'var(--accent-primary)';
                                byteSpan.style.fontWeight = '700';
                                byteSpan.style.textShadow = '0 0 5px rgba(0, 242, 254, 0.5)';
                            }

                            const intVal = parseInt(byteVal, 16);
                            asciiStr += (intVal >= 32 && intVal < 127) ? String.fromCharCode(intVal) : '.';
                        } else {
                            byteSpan.innerText = "  ";
                            asciiStr += " ";
                        }
                        wordSpan.appendChild(byteSpan);
                        if (j < 3) {
                            wordSpan.appendChild(document.createTextNode(" "));
                        }
                    }
                    bytesCol.appendChild(wordSpan);
                    if (group < 3) {
                        bytesCol.appendChild(document.createTextNode("  "));
                    }
                }
                
                row.appendChild(bytesCol);

                // ASCII Column
                const asciiCol = document.createElement('span');
                asciiCol.className = 'hex-ascii';
                asciiCol.innerText = asciiStr;
                row.appendChild(asciiCol);

                container.appendChild(row);
            }
        }

        // DECODER AND RESOLVER FOR ARM32 / THUMB INSTRUCTIONS
        function decodeInstruction(op, trAddr, off, bytes, isThumb) {
            let text = "";
            let target = null;
            let label = "";

            if (isThumb) {
                // ldr rD, [pc, #imm]
                if ((op & 0xF800) === 0x4800) {
                    const rD = (op >> 8) & 7;
                    const imm = (op & 0xFF) * 4;
                    const target_off = (off & ~2) + 4 + imm;
                    text = `ldr r${rD}, [pc, #${imm}]`;
                    
                    if (target_off + 4 <= bytes.length) {
                        target = bytes[target_off] | (bytes[target_off+1] << 8) | (bytes[target_off+2] << 16) | (bytes[target_off+3] << 24);
                    }
                }
                // b<cond> #imm
                else if ((op & 0xF000) === 0xD000) {
                    const cond = (op >> 8) & 0xF;
                    if (cond < 14) {
                        let imm = op & 0xFF;
                        if (imm & 0x80) imm = imm - 256;
                        target = trAddr + 4 + imm * 2;
                        text = `b${getCondition(cond)} 0x${target.toString(16)}`;
                    } else if (cond === 14) {
                        text = `undefined`;
                    } else {
                        text = `svc #${op & 0xFF}`;
                    }
                }
                // b #imm
                else if ((op & 0xF800) === 0xE000) {
                    let imm = op & 0x7FF;
                    if (imm & 0x400) imm = imm - 2048;
                    target = trAddr + 4 + imm * 2;
                    text = `b 0x${target.toString(16)}`;
                }
                // push
                else if ((op & 0xFE00) === 0xB400) {
                    text = `push {${getRegList(op & 0xFF, (op >> 8) & 1)}}`;
                }
                // pop
                else if ((op & 0xFE00) === 0xBC00) {
                    text = `pop {${getRegList(op & 0xFF, (op >> 8) & 1)}}`;
                }
                // bx
                else if ((op & 0xFF00) === 0x4700) {
                    text = `bx r${(op >> 3) & 0xF}`;
                }
                // movs
                else if ((op & 0xF800) === 0x2000) {
                    text = `movs r${(op >> 8) & 7}, #${op & 0xFF}`;
                }
                // add sp
                else if ((op & 0xFE00) === 0xB000) {
                    text = `${(op & 0x80) ? 'sub' : 'add'} sp, #${(op & 0x7F) * 4}`;
                }
                // add rd, sp
                else if ((op & 0xF000) === 0xA000) {
                    text = `add r${(op >> 8) & 7}, sp, #${(op & 0xFF) * 4}`;
                }
                else {
                    text = `.short 0x${op.toString(16).toUpperCase()}`;
                }
            } else {
                // ARM32 mode
                // b #offset
                if ((op & 0x0E000000) === 0x0A000000) {
                    let imm = op & 0x00FFFFFF;
                    if (imm & 0x00800000) imm = imm - 16777216;
                    target = trAddr + 8 + imm * 4;
                    const isLink = (op & 0x01000000) !== 0;
                    text = `${isLink ? 'bl' : 'b'} 0x${target.toString(16)}`;
                }
                // stmfd
                else if ((op & 0x0FC00000) === 0x092D0000) {
                    text = `stmfd sp!, {${getRegList(op & 0xFFFF, 0)}}`;
                }
                // ldmfd
                else if ((op & 0x0FC00000) === 0x08BD0000) {
                    text = `ldmfd sp!, {${getRegList(op & 0xFFFF, 0)}}`;
                }
                // add
                else if ((op & 0x0DE00000) === 0x02800000) {
                    text = `add r${(op >> 12) & 0xF}, r${(op >> 16) & 0xF}, #imm`;
                }
                // sub
                else if ((op & 0x0DE00000) === 0x02400000) {
                    text = `sub r${(op >> 12) & 0xF}, r${(op >> 16) & 0xF}, #imm`;
                }
                else {
                    text = `.word 0x${op.toString(16).toUpperCase().padStart(8, '0')}`;
                }
            }

            // Resolve target label if applicable
            if (target !== null && target !== undefined) {
                const checkAddr = (target & 1) ? (target & ~1) : target;
                const sym = parsedElf.symbols.find(s => s.value === checkAddr || s.value === target);
                if (sym && sym.name) {
                    label = sym.name;
                } else {
                    const sec = parsedElf.section_headers.find(s => s.addr !== 0 && checkAddr >= s.addr && checkAddr < (s.addr + s.size));
                    if (sec) {
                        label = `${sec.name}+0x${(checkAddr - sec.addr).toString(16)}`;
                    }
                }
            }

            return { text, target, label };
          }
