/** @type {import('tailwindcss').Config} */
module.exports = {
  content: ['./index.html'],
  theme: {
    extend: {
      fontFamily: {
        sans: ['Poppins', 'sans-serif'],
      },
      colors: {
        surface: {
          DEFAULT: 'rgba(13, 13, 15, 0.92)',
          solid: '#0D0D0F',
          card: '#141416',
          raised: '#1A1A1D',
          hover: '#222225',
          active: '#2A2A2E',
        },
        amber: {
          DEFAULT: '#D4A044',
          light: '#E5B454',
          dark: '#B8863A',
          dim: '#8B6A30',
          glow: 'rgba(212, 160, 68, 0.15)',
          border: 'rgba(212, 160, 68, 0.2)',
        },
        muted: {
          DEFAULT: '#8A847C',
          light: '#A8A29E',
          dark: '#5C574F',
        },
        danger: {
          DEFAULT: '#C45454',
          hover: '#D46464',
        },
      },
      borderRadius: {
        panel: '10px',
      },
      boxShadow: {
        panel: '0 8px 32px rgba(0, 0, 0, 0.5), 0 0 1px rgba(212, 160, 68, 0.1)',
        glow: '0 0 20px rgba(212, 160, 68, 0.08)',
        'btn-hover': '0 0 12px rgba(212, 160, 68, 0.1)',
      },
    },
  },
  plugins: [],
}
