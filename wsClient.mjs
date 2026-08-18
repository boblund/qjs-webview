export { newWsClient };


import * as os from 'os';
import { Client } from 'socket.so';
import { WsEndpoint } from './wsEndpoint.mjs';
import { parseUrl } from './parseUrl.mjs';
import { TextEncoder, TextDecoder } from './EncodeDecode.mjs';

const enc = new TextEncoder();
const dec = new TextDecoder();

// wsClient.mjs
function newWsClient( url, token = undefined ) {
	const { protocol, addr, port, path } = parseUrl( url );
	const host = addr;
	const wsPath = path ?? '/';
	const req = [
		`GET ${ wsPath } HTTP/1.1`,
		`Host: ${ host }`,
		"Connection: Upgrade",
		"Upgrade: websocket",
		"Sec-WebSocket-Version: 13",
		"Sec-WebSocket-Key: IS0tZXhhbXBsZS5haS0tIQ==",
		"Origin: localhost",
		`token: ${ token }`,
		"",  // blank line = end of headers
		""   // produces trailing \r\n
	].join( "\r\n" );

	const socket = new Client();
	const fds = socket.connect( {
		port: port
			? port
			: protocol == 'wss'
				? 443
				: 80,
		host: addr,
		tls: protocol == 'wss' ? true : false
	} );
	if ( fds === undefined ) throw { error: 'socket.connect failed' };

	const encoded = enc.encode( req );
	os.write( fds[1], encoded.buffer, 0, encoded.byteLength );

	const readBuf = new Uint8Array( 4096 );
	while ( true ) {
		const n = os.read( fds[0], readBuf.buffer, 0, readBuf.length );
		if ( n === 0 || ( n === 1 && readBuf[0] === 0 ) ) {
			throw { error: 'connection closed during handshake' };
		}
		if ( n > 0 ) {
			const chunkText = dec.decode( readBuf.slice( 0, n ) ).split( '\r\n' );
			const [ , code, reason ] = chunkText[0].split( ' ' );
			if ( code == '101' ) {
				console.log( `wsClient code: 101` );
				return new WsEndpoint( fds, socket, 'client' );   // no Promise needed at all
			}
			console.log( `wsClient code, reason, message:`, code, reason, chunkText[chunkText.length - 1] );
			throw { code, reason, message: chunkText[chunkText.length - 1] };
		}
	}
}

/*
function newWsClient( url, token = undefined ){
	const { protocol, addr, port, path } = parseUrl( url );
	const host = addr;
	const wsPath = path ?? '/';
	const req = [
		`GET ${ wsPath } HTTP/1.1`,
		`Host: ${ host }`,
		"Connection: Upgrade",
		"Upgrade: websocket",
		"Sec-WebSocket-Version: 13",
		"Sec-WebSocket-Key: IS0tZXhhbXBsZS5haS0tIQ==",
		"Origin: localhost",
		`token: ${ token }`,
		"",  // blank line = end of headers
		""   // produces trailing \r\n
	].join( "\r\n" );

	let socket = new Client();

	return new Promise( ( res, rej ) => {
		let readBuf = new Uint8Array( 4096 );
		let { protocol, addr, port } = parseUrl( url );
		let fds = socket.connect( {
			port: port
				? port
				: protocol == 'wss'
					? 443
					: 80,
			host: addr,
			tls: protocol == 'wss' ? true : false
		} );

		if( fds === undefined ){
			console.log( 'socket.connect failed' );
			// rej
		}

		os.setReadHandler( fds[ 0 ], () => {
			const n = os.read( fds[ 0 ], readBuf.buffer, 0, readBuf.length );
			if( n === 0 || ( n === 1 && readBuf[0] === 0 ) ){
				os.setReadHandler( fds[ 0 ], null );
				// rej
				return;
			}
			if ( n > 0 ){
				const chunk = readBuf.slice( 0, n );
				const chunkText = dec.decode( chunk ).split( '\r\n' );
				const [ , code, reason ] = chunkText[0].split( ' ' );
				if(  code == '101' ){
					console.log( 'ws upgrade' );
					res( new WsEndpoint( fds, socket, 'client' ) );
				} else {
					os.setReadHandler( fds[ 0 ], null );
					rej( { code, reason, message: chunkText[ chunkText.length - 1 ] } );
				}
				return;
			}
			os.close( fds[ 0 ] );
			os.setReadHandler( fds[ 0 ], null );
		} );

		let encoded = enc.encode( req );
		let buf = encoded.slice().buffer;
		os.write( fds[ 1 ], buf, 0, buf.byteLength );
	} );
};
*/