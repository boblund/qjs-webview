import * as std from 'std';
import { Webview } from './webview.so';

if ( scriptArgs.length != 2 ) {
	console.log( `Usage: ${ scriptArgs[0] } <absolute_path_to_file.html>` );
	std.exit( 1 );
}

const filename = scriptArgs[1];
const w = new Webview( 1 );

w.setTitle( 'Webview Example' );
w.setSize( 480, 320 );
w.navigate( `file://${ filename }` );
w.run();
w.destroy();
