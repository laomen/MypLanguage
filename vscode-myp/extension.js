// MYP Language VS Code Extension
// Launches myp_lsp as a background LSP server for diagnostics, completion, hover, etc.

const vscode = require('vscode');
const path = require('path');
const fs = require('fs');
const cp = require('child_process');

/**
 * Find the myp_lsp executable by searching relative to the extension,
 * the workspace, and common build locations.
 */
function findLspPath(context) {
    const configured = vscode.workspace.getConfiguration('myp').get('lspPath');
    if (configured && fs.existsSync(configured)) return configured;

    // Search locations
    const candidates = [
        // Relative to extension
        path.join(context.extensionPath, '..', 'build', 'myp_lsp'),
        // Workspace root
        path.join(vscode.workspace.rootPath || '', 'build', 'myp_lsp'),
        // Absolute paths
        '/home/xlkj/code/MYPLanguage/build/myp_lsp',
    ];
    for (const c of candidates) {
        try {
            if (fs.existsSync(c)) return c;
        } catch (_) {}
    }
    return 'myp_lsp'; // hope it's on PATH
}

/**
 * Find the stdlib directory relative to the compiler or extension.
 */
function findStdlibPath(context, compilerPath) {
    const configured = vscode.workspace.getConfiguration('myp').get('stdlibPath');
    if (configured && fs.existsSync(configured)) return configured;

    const candidates = [
        path.join(path.dirname(compilerPath), '..', 'stdlib'),
        path.join(context.extensionPath, '..', 'stdlib'),
        path.join(vscode.workspace.rootPath || '', 'stdlib'),
        '/home/xlkj/code/MYPLanguage/stdlib',
    ];
    for (const c of candidates) {
        try {
            if (fs.existsSync(path.join(c, 'env.myp'))) return c;
        } catch (_) {}
    }
    return '';
}

let client = null;

async function activate(context) {
    const lspPath = findLspPath(context);
    const stdlibPath = findStdlibPath(context, lspPath);

    if (!fs.existsSync(lspPath)) {
        vscode.window.showWarningMessage(
            `MYP LSP server not found at "${lspPath}". ` +
            'Syntax highlighting will work, but diagnostics/completion/hover require building the compiler first.'
        );
        // Still register syntax – no LSP
        return;
    }

    // Register restart command
    context.subscriptions.push(
        vscode.commands.registerCommand('myp.restartLsp', async () => {
            if (client) {
                client.stop();
                client = null;
            }
            vscode.window.showInformationMessage('MYP Language Server restarted');
            startClient(context, lspPath, stdlibPath);
        })
    );

    startClient(context, lspPath, stdlibPath);
}

function startClient(context, lspPath, stdlibPath) {
    const serverOptions = {
        command: lspPath,
        args: stdlibPath ? ['--stdlib', stdlibPath] : [],
        options: { stdio: 'pipe' }
    };

    const clientOptions = {
        documentSelector: [{ scheme: 'file', language: 'myp' }],
        synchronize: {
            // Synchronize the workspace root's myp_packages directory
            fileEvents: vscode.workspace.createFileSystemWatcher('**/*.myp')
        },
        diagnosticCollectionName: 'myp',
        traceOutputChannel: vscode.window.createOutputChannel('MYP LSP Trace'),
    };

    // Enable LSP logging if configured
    const traceSetting = vscode.workspace.getConfiguration('myp').get('trace.server');
    if (traceSetting && traceSetting !== 'off') {
        clientOptions.traceOutputChannel = vscode.window.createOutputChannel('MYP LSP Trace');
    }

    const lspClient = require('vscode-languageclient/node').LanguageClient;
    client = new lspClient(
        'myp-language-server',
        'MYP Language Server',
        serverOptions,
        clientOptions
    );

    client.onDidChangeState((e) => {
        if (e.newState === 1) { // stopped
            client = null;
        }
    });

    client.start();
    console.log('MYP LSP client started');
}

function deactivate() {
    if (client) {
        return client.stop();
    }
}

module.exports = { activate, deactivate };
