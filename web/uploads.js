// SPDX-License-Identifier: GPL-3.0-or-later
'use strict';

// Only one browser upload runs at a time. The console processes committed files
// through the existing queue while the next file is being transferred.
(() => {
    const get = id => document.getElementById(id);
    const dialog = makeDialog('uploadOverlay', 'selectUploadFiles');
    let files = [],
    busy = false,
    reading = false,
    cancelled = false,
    currentId,
    xhr;
    let selectedBytes = 0,
    uploadedBytes = 0,
    queued = [];
    const supported = file => /\.(nsp|nsz|xci)$/i.test(file.name);

    function controls() {
        ['selectUploadFiles', 'selectUploadFolder'].forEach(id => get(id).disabled = busy || reading);
        get('startUpload').disabled = busy || reading || !files.length;
        get('clearUpload').disabled = busy || reading || !files.length;
        get('cancelUpload').hidden = !busy;
        get('closeUpload').textContent = busy ? 'Hide' : 'Close';
        get('uploadDrop').setAttribute('aria-disabled', String(busy || reading));
    }
    function selection() {
        selectedBytes = files.reduce((sum, file) => sum + file.size, 0);
        get('uploadSummary').textContent = files.length ? files.length + (files.length === 1 ? ' file · ' : ' files · ') + sizeLabel(selectedBytes) : 'No files selected.';
        get('uploadSelection').replaceChildren();
        files.slice(0, 20).forEach(file => get('uploadSelection').append(element('li', (file.uploadPath || file.webkitRelativePath || file.name) + ' · ' + sizeLabel(file.size))));
        if (files.length > 20)
            get('uploadSelection').append(element('li', '… and ' + (files.length - 20) + ' more'));
        controls();
    }
    function addFiles(incoming) {
        const known = new Set(files.map(file => (file.uploadPath || file.webkitRelativePath || file.name) + ':' + file.size + ':' + file.lastModified));
        let skipped = 0;
        for (const file of incoming) {
            if (!supported(file) || !file.size) {
                skipped++;
                continue;
            }
            const key = (file.uploadPath || file.webkitRelativePath || file.name) + ':' + file.size + ':' + file.lastModified;
            if (known.has(key))
                continue;
            if (files.length >= 500)
                throw new Error('Choose up to 500 game files per upload.');
            known.add(key);
            files.push(file);
        }
        selection();
        get('uploadProgress').hidden = true;
        get('uploadStatus').textContent = skipped ? skipped + ' unrelated or empty ' + (skipped === 1 ? 'file' : 'files') + ' skipped.' : '';
    }
    async function directoryFiles(entry, output, depth = 0) {
        if (depth > 32)
            throw new Error('Folder nesting is too deep.');
        if (entry.isFile) {
            const file = await new Promise((resolve, reject) => entry.file(resolve, reject));
            if (supported(file)) {
                file.uploadPath = entry.fullPath;
                output.push(file);
            }
            if (output.length > 500)
                throw new Error('Choose up to 500 game files per upload.');
        } else if (entry.isDirectory) {
            const reader = entry.createReader();
            while (true) {
                const children = await new Promise((resolve, reject) => reader.readEntries(resolve, reject));
                if (!children.length)
                    break;
                for (const child of children)
                    await directoryFiles(child, output, depth + 1);
            }
        }
    }
    async function acceptDrop(transfer) {
        if (busy || reading)
            return;
        reading = true;
        controls();
        get('uploadError').textContent = '';
        get('uploadStatus').textContent = 'Reading files…';
        try {
            // Capture entry handles before the browser invalidates the drop event.
            const entries = [...transfer.items || []].map(item => item.webkitGetAsEntry ? item.webkitGetAsEntry() : null).filter(Boolean);
            const fallback = [...transfer.files];
            const incoming = [];
            if (entries.length)
                for (const entry of entries)
                    await directoryFiles(entry, incoming);
            else
                incoming.push(...fallback);
            addFiles(incoming);
        } catch (error) {
            get('uploadError').textContent = error.message;
            selection();
        } finally {
            reading = false;
            controls();
        }
    }
    function progress(current = 0, name = '') {
        const bytes = uploadedBytes + current;
        const text = (name ? name + ' · ' : '') + sizeLabel(bytes) + ' / ' + sizeLabel(selectedBytes);
        get('uploadStatus').textContent = text;
        get('uploadActivityText').textContent = 'Uploading · ' + text;
        [get('uploadProgress'), get('uploadActivityProgress')].forEach(bar => {
            bar.max = Math.max(1, selectedBytes);
            bar.value = bytes;
        });
    }
    function chunk(id, offset, blob, name) {
        return new Promise((resolve, reject) => {
            const request = new XMLHttpRequest();
            xhr = request;
            request.open('PUT', '/api/uploads/' + id);
            request.setRequestHeader('Content-Type', 'application/octet-stream');
            request.setRequestHeader('X-Upload-Offset', String(offset));
            request.timeout = 120000;
            request.upload.onprogress = event => progress(offset + event.loaded, name);
            request.onload = () => {
                xhr = undefined;
                try {
                    const result = JSON.parse(request.responseText);
                    if (request.status < 200 || request.status >= 300)
                        throw new Error(result.error || 'Upload failed.');
                    if (result.received !== offset + blob.size)
                        throw new Error('Unexpected upload progress from console.');
                    resolve();
                } catch (error) {
                    reject(error);
                }
            };
            request.onerror = () => {
                xhr = undefined;
                reject(new Error('Connection lost while uploading.'));
            };
            request.ontimeout = () => {
                xhr = undefined;
                reject(new Error('Upload timed out.'));
            };
            request.onabort = () => {
                xhr = undefined;
                reject(new Error('Upload cancelled.'));
            };
            request.send(blob);
        });
    }
    async function start() {
        if (busy || reading || !files.length)
            return;
        busy = true;
        cancelled = false;
        uploadedBytes = 0;
        queued = [];
        controls();
        get('uploadError').textContent = '';
        get('uploadProgress').hidden = false;
        get('uploadActivity').hidden = false;
        progress();
        try {
            for (const file of files) {
                if (cancelled)
                    throw new Error('Upload cancelled.');
                const session = await api('uploads', {
                    method: 'POST',
                    body: JSON.stringify({
                        name: file.name,
                        size: file.size
                    })
                });
                currentId = session.id;
                for (let offset = 0; offset < file.size; offset += session.chunkSize) {
                    if (cancelled)
                        throw new Error('Upload cancelled.');
                    await chunk(session.id, offset, file.slice(offset, Math.min(file.size, offset + session.chunkSize)), file.name);
                }
                if (cancelled)
                    throw new Error('Upload cancelled.');
                const result = await api('uploads/' + session.id + '/commit', {
                    method: 'POST',
                    body: '{}'
                });
                queued.push(result.id);
                currentId = undefined;
                uploadedBytes += file.size;
                progress(0, file.name);
                await refreshDownloads().catch(() => {});
            }
            if (cancelled)
                throw new Error('Upload cancelled.');
            get('uploadStatus').textContent = 'Uploaded ' + files.length + (files.length === 1 ? ' file.' : ' files.') + ' Inspection and conversion continue in Downloads.';
            files = [];
            selection();
        } catch (error) {
            get('uploadError').textContent = error.message + ' ' + (queued.length ? 'Files already submitted are listed in Downloads.' : '');
        } finally {
            if (currentId) {
                await api('uploads/' + currentId + '/cancel', {
                    method: 'POST',
                    body: '{}'
                }).catch(() => {});
                currentId = undefined;
            }
            if (cancelled)
                await Promise.all(queued.map(id => api('downloads/' + id + '/cancel', {
                            method: 'POST',
                            body: '{}'
                        }).catch(() => {})));
            busy = false;
            controls();
            get('uploadActivity').hidden = true;
            refreshDownloads().catch(() => {});
        }
    }

    get('uploadButton').onclick = () => dialog.open(get('uploadButton'));
    get('uploadActivity').onclick = () => dialog.open(get('uploadActivity'));
    get('closeUpload').onclick = () => dialog.close();
    get('selectUploadFiles').onclick = () => get('uploadFiles').click();
    get('selectUploadFolder').onclick = () => get('uploadFolder').click();
    ['uploadFiles', 'uploadFolder'].forEach(id => get(id).onchange = () => {
        if (busy || reading)
            return;
        get('uploadError').textContent = '';
        try {
            addFiles([...get(id).files]);
        } catch (error) {
            get('uploadError').textContent = error.message;
            selection();
        }
        get(id).value = '';
    });
    const drop = get('uploadDrop');
    drop.onclick = () => {
        if (!busy && !reading)
            get('uploadFiles').click();
    };
    drop.onkeydown = event => {
        if (event.key === 'Enter' || event.key === ' ') {
            event.preventDefault();
            drop.click();
        }
    };
    ['dragenter', 'dragover', 'dragleave', 'drop'].forEach(type => drop.addEventListener(type, event => event.preventDefault()));
    drop.ondragover = () => {
        if (!busy && !reading)
            drop.classList.add('dragging');
    };
    drop.ondragleave = () => drop.classList.remove('dragging');
    drop.ondrop = event => {
        drop.classList.remove('dragging');
        acceptDrop(event.dataTransfer);
    };
    get('startUpload').onclick = start;
    get('cancelUpload').onclick = () => {
        cancelled = true;
        if (xhr)
            xhr.abort();
    };
    get('clearUpload').onclick = () => {
        files = [];
        selection();
        get('uploadError').textContent = '';
        get('uploadStatus').textContent = '';
        get('uploadProgress').hidden = true;
    };
    window.addEventListener('beforeunload', event => {
        if (busy) {
            event.preventDefault();
            event.returnValue = '';
        }
    });
    window.addEventListener('pagehide', () => {
        if (xhr)
            xhr.abort();
        if (currentId && navigator.sendBeacon)
            navigator.sendBeacon('/api/uploads/' + currentId + '/cancel', new Blob(['{}'], {
                    type: 'application/json'
                }));
    });
    selection();
})();
