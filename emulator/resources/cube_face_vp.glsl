uniform float LCD_SIZE;
const float HEIGHT = 0.6;

varying vec2 lcdCoord;
varying vec2 hilightCoord;

void main() {
     vec4 transformed = gl_ModelViewProjectionMatrix * gl_Vertex;

     vec4 center = gl_ModelViewProjectionMatrix * vec4(0.0, 0.0, HEIGHT, 1.0);
     vec4 sizeVec = abs(gl_ModelViewProjectionMatrix * vec4(1.0, 1.0, HEIGHT, 1.0) - center);
     float size = max(sizeVec.x, sizeVec.y);
     vec2 hl = (transformed.xy - center.xy) / size * 0.25;

     gl_Position = transformed;
     lcdCoord = gl_Vertex.xy * (0.5 / LCD_SIZE);
     hilightCoord = vec2(hl.x + 0.5, -hl.y + 0.5);
}
