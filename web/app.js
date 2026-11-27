"use strict";

const byId = id => document.getElementById(id);
const workspace = byId("workspace");
const editor = byId("editor");
const prompt = byId("prompt");
const login = byId("login");
const commandDialog = byId("commands");
const modelDialog = byId("model-menu");
const content = byId("content");
const drafts = new Map();
let token = "", feedback = "", state, commands = [], revision = 0, mutating = false;
let editorId = null, libraryKey = "", messageKey = "", panelKey = "", toolMode;
let earlier = null, uploadKind, mutation = Promise.resolve(), streamText = "";
let modelRequested = false, modelSeeded = false, modelEditing = "";

function element(tag, text, className) {
    const node = document.createElement(tag);
    if (text !== undefined) node.textContent = text;
    if (className) node.className = className;
    return node;
}

function button(label, handle) {
    const node = element("button", label);
    node.type = "button";
    node.addEventListener("click", handle);
    return node;
}

async function request(route, body) {
    const headers = {"X-Neutrino-Web": "1"};
    if (token) headers.Authorization = "Bearer " + token;
    const options = {headers, cache: "no-store"};
    if (body !== undefined) {
        options.method = "POST";
        if (body instanceof File) options.body = body;
        else {
            headers["Content-Type"] = "application/json";
            options.body = JSON.stringify(body);
        }
    }
    const response = await fetch("/api/" + route, options);
    if (response.status === 401) {
        token = "";
        if (!login.open) login.showModal();
        throw new Error("Enter the browser access token.");
    }
    if (!response.ok) throw new Error(await response.text());
    return response;
}

function alertText(error) {
    const location = login.open ? "login-error" : "status";
    if (!login.open) feedback = error.message;
    byId(location).textContent = error.message;
}

async function refresh() {
    const before = revision;
    try {
        const result = await (await request("state")).json();
        if (feedback === "Failed to fetch") feedback = "";
        if (before === revision && !mutating) render(result);
    } catch (error) {
        if (before === revision) alertText(error);
    }
}

async function poll() {
    if (!mutating && !login.open) await refresh();
    setTimeout(poll, state?.busy || state?.researching ? 220 : 1100);
}

function mutate(route, payload, beforeRender) {
    ++revision;
    mutating = true;
    const requestRevision = revision;
    mutation = mutation.then(async () => {
        feedback = "";
        try {
            const next = await (await request(route, payload)).json();
            if (beforeRender) beforeRender();
            render(next);
            return true;
        } catch (error) {
            alertText(error);
            return false;
        } finally {
            if (requestRevision === revision) mutating = false;
        }
    });
    return mutation;
}

const act = (action, fields) => mutate("action", {action, ...fields});
function submit(text) {
    if (text.trim() !== "/model") return act("submit", {text});
    modelRequested = true;
    modelSeeded = false;
    modelEditing = "";
    return act("submit", {text}).then(ok => {
        if (!ok || !state?.modelMenu) modelRequested = false;
        return ok;
    });
}

function writeModelMenu(menu) {
    if (!menu || !modelRequested) {
        if (modelDialog.open) modelDialog.close();
        if (!menu) modelRequested = false;
        modelSeeded = false;
        return;
    }
    if (!modelDialog.open) modelDialog.showModal();
    const provider = byId("model-provider"), url = byId("model-url"), id = byId("model-id"), key = byId("model-key");
    const usage = byId("model-usage");
    const initialize = !modelSeeded && !menu.busy;
    if (initialize) {
        provider.querySelectorAll("[data-saved]").forEach(option => option.remove());
        for (const [i, name] of (menu.profiles ?? []).entries()) {
            const option = element("option", name);
            option.value = String(5 + i);
            option.dataset.saved = "";
            provider.append(option);
        }
        provider.value = "0";
        url.value = menu.endpoint;
        id.value = menu.model;
        key.value = "";
        byId("model-name").value = "";
        usage.checked = menu.usage;
        modelSeeded = true;
    }
    const saved = Number(provider.value) >= 5 && !modelEditing;
    const custom = menu.admin && (provider.value === "4" || !!modelEditing);
    const switching = menu.admin && !saved && (provider.value !== "0" || !!modelEditing);
    byId("model-current-endpoint").textContent = menu.admin && provider.value === "0" ?
        [menu.active_profile, menu.endpoint, menu.authenticated ? "Gateway key configured" : ""].filter(Boolean).join(" · ") : "";
    byId("model-url-label").hidden = url.hidden = !custom;
    byId("model-key-label").hidden = key.hidden = !switching;
    byId("model-usage-label").hidden = !menu.admin || saved;
    byId("model-name-label").hidden = byId("model-name").hidden = !menu.admin || saved;
    provider.disabled = !menu.admin || menu.busy || !!modelEditing;
    usage.disabled = byId("model-name").disabled = !menu.admin || menu.busy || saved;
    url.disabled = !custom || menu.busy;
    key.disabled = !switching || menu.busy;
    id.disabled = menu.busy || saved;
    id.hidden = saved;
    document.querySelector('label[for="model-id"]').hidden = saved;
    byId("model-apply").disabled = menu.busy;
    byId("model-apply").textContent = modelEditing ? "Save changes" : saved ? "Switch provider" : "Apply";
    byId("model-name-label").textContent = modelEditing ? "Name" : "Save as";
    byId("model-name").required = !!modelEditing;
    key.placeholder = modelEditing ? "Blank keeps key · none clears it" : "NEUTRINO_PROVIDER_KEY_NAME";
    for (const button of [byId("model-edit"),byId("model-delete")]) {
        button.hidden = !saved;
        button.disabled = menu.busy;
    }
    id.required = menu.admin && !saved;
    id.placeholder = menu.admin ? "Model ID from your provider" : "Server default";
    url.required = custom;
    byId("model-feedback").textContent = menu.busy ? "Contacting gateway…" :
        menu.error || (menu.admin ? "" : "API settings require gateway admin access.");
    if (initialize) (menu.admin ? provider : id).focus();
}

function closeModelMenu() {
    modelRequested = false;
    modelDialog.close();
    act("model-close");
}

byId("model-close").addEventListener("click",closeModelMenu);
modelDialog.addEventListener("cancel",event => {
    event.preventDefault();
    closeModelMenu();
});
byId("model-provider").addEventListener("change",() => writeModelMenu(state?.modelMenu));
function selectedProfile() {
    return state?.modelMenu?.profiles?.[Number(byId("model-provider").value)-5];
}
byId("model-edit").addEventListener("click",() => {
    const name = selectedProfile();
    const details = state?.modelMenu?.profile_details;
    if (!name || !details || !Object.hasOwn(details,name)) return;
    modelEditing = name;
    byId("model-url").value = details[name].endpoint;
    byId("model-id").value = details[name].model;
    byId("model-usage").checked = details[name].stream_usage;
    byId("model-key").value = "";
    byId("model-name").value = name;
    writeModelMenu(state.modelMenu);
    byId("model-name").focus();
});
byId("model-delete").addEventListener("click",() => {
    const profile = selectedProfile();
    if (profile && window.confirm("Delete the saved provider “"+profile+"”?"))
        act("model-profile-delete",{profile});
});
byId("model-form").addEventListener("submit",event => {
    event.preventDefault();
    act(modelEditing ? "model-profile-edit" : "model-apply",{
        profile: modelEditing,
        provider: Number(byId("model-provider").value), endpoint: byId("model-url").value.trim(),
        model: byId("model-id").value.trim(), key: byId("model-key").value.trim(), usage: byId("model-usage").checked,
        name: byId("model-name").value.trim()
    });
});

function drawer(name, show) {
    workspace.dataset[name] = String(show);
    byId(name + "-toggle").setAttribute("aria-expanded", String(show));
    if (show) {
        const other = name === "library" ? "tools" : "library";
        workspace.dataset[other] = "false";
        byId(other + "-toggle").setAttribute("aria-expanded", "false");
    }
}

function openItem(roleplay, id) {
    earlier = null;
    drawer("library", false);
    act("open", {roleplay, id});
}

function writeLibrary(next) {
    const key = JSON.stringify([next.roleplay, next.id, next.library]);
    if (key === libraryKey) return;
    libraryKey = key;
    const items = byId("items");
    items.replaceChildren();
    const rows = next.roleplay ? next.library : [{id: "", title: "Scratchpad"}, ...next.library];
    for (const item of rows) {
        const row = button(item.title, () => openItem(next.roleplay, item.id));
        row.setAttribute("aria-current", String(item.id === next.id));
        items.append(row);
    }
    if (!rows.length) items.append(element("p", "Create a story to begin.", "empty"));
}

function makeMessage(message) {
    const article = element("article", undefined, "message");
    article.dataset.role = message.role;
    article.append(element("h3", message.speaker || (message.role === "user" ? "Player" : "Narrator")),
                   prose(message.text));
    return article;
}

function prose(text) {
    const paragraph = element("p");
    const links = /\[([^\]\n]{1,200})\]\((https?:\/\/[^\s)<>]+)\)|(https?:\/\/[^\s<>\])]+)/gi;
    let offset = 0;
    for (const match of text.matchAll(links)) {
        const title = match[1];
        const url = match[2] || match[3].replace(/[.,;!?]+$/, "");
        paragraph.append(text.slice(offset, match.index));
        const link = element("a", title || url);
        link.href = url;
        link.target = "_blank";
        link.rel = "noreferrer noopener";
        paragraph.append(link);
        offset = match.index + (title ? match[0].length : url.length);
    }
    paragraph.append(text.slice(offset));
    return paragraph;
}

function writeMessages(next) {
    const old = earlier?.id === next.id ? earlier.messages : [];
    const first = next.messages[0]?.id;
    const prefix = first && old.some(item => item.id === first) ?
        old.slice(0, old.findIndex(item => item.id === first)) : [];
    const messages = [...prefix, ...next.messages];
    const key = JSON.stringify([next.id, messages]);
    const nearEnd = content.scrollHeight - content.scrollTop - content.clientHeight < 85;
    if (key !== messageKey) {
        messageKey = key;
        byId("messages").replaceChildren(...messages.map(makeMessage));
        if (!messages.length) byId("messages").append(element("p", "Write the first action or choose Narrate.", "empty"));
    }
    byId("earlier").hidden = !next.hasHistory || earlier?.id === next.id;
    const stream = byId("stream");
    stream.hidden = !next.stream;
    if (next.stream && streamText !== next.stream)
        stream.replaceChildren(makeMessage({role: "assistant", text: next.stream}));
    streamText = next.stream;
    if (nearEnd) content.scrollTop = content.scrollHeight;
}

function writeEditor(next) {
    const draft = drafts.get(next.id);
    const wanted = (draft ? draft.text : next.manuscript).replace(/\r\n?/g, "\n");
    if (editorId !== next.id || editor.value !== wanted) editor.value = wanted;
    editorId = next.id;
    byId("save").disabled = !draft;
}

function median(values) {
    if (!values.length) return null;
    values.sort((a, b) => a - b);
    return (values[Math.floor((values.length - 1) / 2)] + values[Math.floor(values.length / 2)]) / 2;
}

function telemetry(value) {
    const div = element("div");
    const server = value.server;
    div.append(element("p", server ? "Gateway online" : value.error || "Checking gateway…"));
    div.append(element("p", "Physics scheduler · " + value.physicsScheduler));
    if (server) div.append(element("p", server.model || "Default model"));
    const metrics = element("div", undefined, "metrics");
    for (const [key, name, unit, digits] of [["rate", "Median throughput", " tok/s", 1], ["ttft", "Median first token", " ms", 0]]) {
        const tile = element("div");
        const values = value.samples.map(sample => sample[key]).filter(Number.isFinite);
        const reading = median(values);
        tile.append(element("span", name), element("strong", reading === null ? "—" : reading.toFixed(digits) + unit),
                    element("small", values.length + " measured"));
        metrics.append(tile);
    }
    div.append(metrics, element("p", value.samples.length + " recent generations"));
    if (server) div.append(element("p", (server.active ?? 0) + " active · " + (server.queued ?? 0) + " queued"));
    return div;
}

function writePanel(panel, busy) {
    const key = JSON.stringify([panel, busy]);
    if (key === panelKey) return;
    panelKey = key;
    byId("panel").hidden = !panel;
    if (!panel) return;
    byId("panel-title").textContent = panel.operation ?
        panel.operation + " · " + panel.candidate + " of " + panel.count : panel.title;
    const body = byId("panel-body");
    const actions = byId("panel-actions");
    body.replaceChildren(panel.neofetch ? telemetry(panel.neofetch) : element("pre", panel.text || (busy ? "Generating…" : "No text")));
    actions.replaceChildren();
    if (panel.operation) {
        if (panel.candidate > 1) actions.append(button("Previous", () => submit("/preview " + (panel.candidate - 1))));
        if (panel.candidate < panel.count) actions.append(button("Next", () => submit("/preview " + (panel.candidate + 1))));
        if (panel.canApply) actions.append(button("Apply", () => submit("/accept")));
        if (!busy) actions.append(button("Discard", () => submit("/reject")));
        actions.append(button("Download", () => download("candidate", "neutrino-candidate.txt")));
    } else if (panel.title === "Prompt trace") actions.append(button("Download", () => download("trace", "neutrino-trace.txt")));
}

function quickTools(roleplay) {
    if (toolMode === roleplay) return;
    toolMode = roleplay;
    const tools = roleplay ? [
        ["Narrate", "/narrate"], ["Swipe", "/swipe"], ["Branches", "/branch"], ["Compendium", "/memory"],
        ["World", "/world"], ["Stats", "/stats"], ["Models", "/model"], ["Prompt trace", "/trace"], ["Neofetch", "/neofetch"]
    ] : [
        ["Continue", "/continue"], ["Revisions", "/revisions"], ["Story Bible", "/bible"],
        ["Context", "/context"], ["Start roleplay", "/play"], ["Models", "/model"], ["Prompt trace", "/trace"], ["Neofetch", "/neofetch"]
    ];
    byId("quick-tools").replaceChildren(...tools.map(([label, command]) => button(label, () => submit(command))));
}

function render(next) {
    const switched = !state || state.id !== next.id || state.roleplay !== next.roleplay;
    const showPanel = next.panel && (!state?.panel || state.panel.title !== next.panel.title);
    state = next;
    commands = next.commands ?? commands;
    byId("title").textContent = next.title;
    byId("section-label").textContent = next.roleplay ? "Roleplay" : "Manuscript";
    byId("library-title").textContent = next.roleplay ? "Stories" : "Manuscripts";
    byId("roleplay-mode").setAttribute("aria-pressed", String(next.roleplay));
    byId("manuscript-mode").setAttribute("aria-pressed", String(!next.roleplay));
    byId("roleplay").hidden = !next.roleplay;
    byId("manuscript").hidden = next.roleplay;
    byId("prompt-label").textContent = next.roleplay ? "Your next action" : "Writing direction";
    const status = feedback || next.status || (next.busy ? "Generating…" : next.researching ? "Researching…" : "");
    if (byId("status").textContent !== status) byId("status").textContent = status;
    byId("cancel").hidden = !next.busy;
    workspace.dataset.focus = String(next.compact);
    if (switched) {
        content.scrollTop = 0;
        earlier = null;
    }
    if (next.roleplay) {
        writeMessages(next);
        if (switched) content.scrollTop = content.scrollHeight;
    } else writeEditor(next);
    writeLibrary(next);
    quickTools(next.roleplay);
    writePanel(next.panel, next.busy);
    writeModelMenu(next.modelMenu);
    if (showPanel) drawer("tools", true);
}

async function download(route, name) {
    try {
        const blob = await (await request(route)).blob();
        const url = URL.createObjectURL(blob);
        const link = element("a");
        link.href = url;
        link.download = name;
        link.click();
        setTimeout(() => URL.revokeObjectURL(url), 1000);
    } catch (error) { alertText(error); }
}

function setPrompt(text) {
    if (commandDialog.open) commandDialog.close();
    drawer("library", false);
    prompt.value = text;
    prompt.focus();
}

function findCommands() {
    const needle = byId("command-search").value.toLowerCase().replace(/^\//, "");
    const fileOnly = new Set(["import", "export", "lore", "preset", "preview-save", "server", "quit"]);
    const rows = commands.filter(item => !fileOnly.has(item.name) &&
        (item.name + " " + item.summary).toLowerCase().includes(needle));
    byId("command-results").replaceChildren(...rows.map(item => {
        const itemButton = button("/" + item.name + item.args, () => {
            if (!item.args || item.name === "model") {
                commandDialog.close();
                submit("/" + item.name);
            } else setPrompt("/" + item.name + " ");
        });
        itemButton.append(element("span", item.summary));
        return itemButton;
    }));
}

function openCommands() {
    byId("command-search").value = "";
    findCommands();
    if (!commandDialog.open) commandDialog.showModal();
    byId("command-search").focus();
}

byId("composer").addEventListener("submit", async event => {
    event.preventDefault();
    const text = prompt.value;
    if (text.trim() && await submit(text) && prompt.value === text) prompt.value = "";
});
prompt.addEventListener("keydown", event => {
    if (event.key === "Enter" && (event.ctrlKey || event.metaKey)) byId("composer").requestSubmit();
});
byId("cancel").onclick = () => act("cancel");
byId("manuscript-mode").onclick = () => openItem(false);
byId("roleplay-mode").onclick = () => openItem(true);
byId("library-toggle").onclick = () => drawer("library", workspace.dataset.library !== "true");
byId("tools-toggle").onclick = () => drawer("tools", workspace.dataset.tools !== "true");
byId("close-panel").onclick = () => { drawer("tools", false); act("close"); };
byId("create").onclick = () => setPrompt(state?.roleplay ? "/story " : "/scene ");
byId("command-toggle").onclick = openCommands;
byId("command-search").oninput = findCommands;
document.addEventListener("keydown", event => {
    if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "p" && !login.open && !modelDialog.open) {
        event.preventDefault();
        openCommands();
    }
});
editor.addEventListener("input", () => {
    if (!state || state.roleplay) return;
    const base = drafts.get(editorId)?.base ?? state.manuscript;
    if (editor.value === base) drafts.delete(editorId);
    else drafts.set(editorId, {base, text: editor.value});
    byId("save").disabled = !drafts.has(editorId);
});
async function saveDraft() {
    const id = editorId;
    const draft = drafts.get(id);
    if (!draft) return true;
    return mutate("action", {action: "edit", id, ...draft}, () => {
        if (drafts.get(id) === draft) drafts.delete(id);
    });
}
byId("save").onclick = saveDraft;
byId("select").onclick = async () => {
    const start = editor.selectionStart, end = editor.selectionEnd;
    if (start === end) { byId("status").textContent = "Select manuscript text first."; return; }
    const encoder = new TextEncoder();
    const offset = n => encoder.encode(editor.value.slice(0, n)).length;
    const selection = "/select " + offset(start) + " " + offset(end);
    if (await saveDraft()) submit(selection);
};
byId("earlier").onclick = async () => {
    const id = state.id;
    try {
        const messages = await (await request("history")).json();
        if (state.id === id && state.roleplay) {
            earlier = {id, messages};
            writeMessages(state);
            content.scrollTop = 0;
        }
    } catch (error) { alertText(error); }
};
document.querySelectorAll("[data-download]").forEach(node => {
    node.onclick = () => download(node.dataset.download, "neutrino-workspace.json");
});
const upload = byId("upload");
document.querySelectorAll("[data-upload]").forEach(node => {
    node.onclick = () => {
        uploadKind = node.dataset.upload;
        upload.accept = uploadKind === "import" ? ".json,.jsonl,.png" : ".json";
        upload.value = "";
        upload.click();
    };
});
upload.onchange = async () => {
    if (upload.files[0] && await mutate(uploadKind, upload.files[0])) drawer("library", false);
};
byId("login-form").addEventListener("submit", async event => {
    event.preventDefault();
    token = byId("token").value;
    try {
        const next = await (await request("state")).json();
        byId("token").value = "";
        byId("login-error").textContent = "";
        login.close();
        render(next);
    } catch (error) { alertText(error); }
});
login.addEventListener("cancel", event => event.preventDefault());
window.addEventListener("beforeunload", event => {
    if (drafts.size) event.preventDefault();
});

poll();
